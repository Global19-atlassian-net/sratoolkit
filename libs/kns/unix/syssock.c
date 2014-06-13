/*===========================================================================
*
*                            PUBLIC DOMAIN NOTICE
*               National Center for Biotechnology Information
*
*  This software/database is a "United States Government Work" under the
*  terms of the United States Copyright Act.  It was written as part of
*  the author's official duties as a United States Government employee and
*  thus cannot be copyrighted.  This software/database is freely available
*  to the public for use. The National Library of Medicine and the U.S.
*  Government have not placed any restriction on its use or reproduction.
*
*  Although all reasonable efforts have been taken to ensure the accuracy
*  and reliability of the software and data, the NLM and the U.S.
*  Government do not and cannot warrant the performance or results that
*  may be obtained by using this software or data. The NLM and the U.S.
*  Government disclaim all warranties, express or implied, including
*  warranties of performance, merchantability or fitness for any particular
*  purpose.
*
*  Please cite the author in any work or product based on this material.
*
* ===========================================================================
*
*/


/*--------------------------------------------------------------------------
 * forwards
 */
#define KSTREAM_IMPL KSocket

#include <kns/extern.h>
#include <kns/manager.h>
#include <kns/socket.h>
#include <kns/impl.h>
#include <kns/endpoint.h>

#include <klib/debug.h> /* DBGMSG */
#include <klib/log.h>
#include <klib/out.h>
#include <klib/printf.h>
#include <klib/rc.h>
#include <klib/text.h>

#include <kproc/timeout.h>

#include "mgr-priv.h"
#include "stream-priv.h"
#include "poll-priv.h"

#include <sysalloc.h>

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <os-native.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>

#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif


/*--------------------------------------------------------------------------
 * KSocket
 *  a socket IS a stream
 *
 *  in Berkeley socket terminology, a STREAM implies a CONTINUOUS stream,
 *  which is implemented by the TCP connection. A "chunked" or discontiguous
 *  stream would be a datagram stream, implemented usually by UDP.
 *
 *  in VDB terminology, a STREAM is a fluid, moving target that is observed
 *  from a stationary point, whereas a FILE or OBJECT is a static stationary
 *  target observed from a movable window. This means that a STREAM cannot be
 *  addressed randomly, whereas a FILE or OBJECT can.
 */
struct KSocket
{
    KStream dad;
    const char * path;
    uint32_t type;
    int32_t read_timeout;
    int32_t write_timeout;
    int fd;
};

LIB_EXPORT rc_t CC KSocketAddRef( struct KSocket *self )
{
    return KStreamAddRef ( & self -> dad );
}

LIB_EXPORT rc_t CC KSocketRelease ( struct KSocket *self )
{
    return KStreamRelease ( & self -> dad );
}

static
rc_t CC KSocketWhack ( KSocket *self )
{
    assert ( self != NULL );

    shutdown ( self -> fd, SHUT_WR );
    
    while ( 1 ) 
    {
        char buffer [ 1024 ];
        ssize_t result = recv ( self -> fd, buffer, sizeof buffer, MSG_DONTWAIT );
        if ( result <= 0 )
            break;
    }

    shutdown ( self -> fd, SHUT_RD );

    close ( self -> fd );

    if ( self -> path != NULL )
    {
        unlink ( self -> path );
        free ( ( void* ) self -> path );
    }
        
    free ( self );

    return 0;
}

static
rc_t HandleErrno ( const char *func_name, unsigned int lineno )
{
    int lerrno;
    rc_t rc = 0;
    
    switch ( lerrno = errno )
    {
    case EACCES: /* write permission denied */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcMemory, rcUnauthorized );            
        break;
    case EADDRINUSE: /* address is already in use */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcMemory, rcExists );
        break;
    case EADDRNOTAVAIL: /* requested address was not local */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcMemory, rcNotFound );
        break;
    case EAGAIN: /* no more free local ports or insufficient rentries in routing cache */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcNoObj, rcExhausted );            
        break;
    case EAFNOSUPPORT: /* address didnt have correct address family in ss_family field */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcName, rcError );            
        break;
    case EALREADY: /* socket is non blocking and a previous connection has not yet completed */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcId, rcUndefined );
        break;
    case EBADF: /* invalid sock fd */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcId, rcInvalid );
        break;
    case ECONNREFUSED: /* remote host refused to allow network connection */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcConnection, rcCanceled );
        break;
    case ECONNRESET: /* connection reset by peer */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcConnection, rcCanceled );
        break;
    case EDESTADDRREQ: /* socket is not connection-mode and no peer address set */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcId, rcInvalid );
        break;
    case EFAULT: /* buffer pointer points outside of process's adress space */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcMemory, rcOutofrange );
        break;
    case EINPROGRESS: /* call is in progress */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcId, rcUndefined );
        break;
    case EINTR: /* recv interrupted before any data available */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcConnection, rcCanceled );
        break;
    case EINVAL: /* invalid argument */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcParam, rcInvalid );
        break;
    case EISCONN: /* connected already */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcConnection, rcExists );
        break;
    case ELOOP: /* too many symbolic links in resolving addr */
        rc = RC ( rcNS, rcNoTarg, rcResolving, rcLink, rcExcessive );
        break;
    case EMFILE: /* process file table overflow */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcNoObj, rcError );
        break;
    case EMSGSIZE: /* msg size too big */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcMessage, rcExcessive );
        break;
    case ENAMETOOLONG: /* addr name is too long */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcName, rcExcessive );
        break;
    case ENETUNREACH: /* network is unreachable */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcConnection, rcNotAvailable );
        break;
    case ENOBUFS: /* output queue for a network connection was full. 
                     ( wont typically happen in linux. Packets are just silently dropped */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcConnection, rcInterrupted );
        break;
    case ENOENT: /* file does not exist */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcId, rcNotFound );
        break;
    case ENOMEM: /* Could not allocate memory */
        rc = RC ( rcNS, rcNoTarg, rcAllocating, rcMemory, rcError );
        break;
    case ENOTCONN: /* socket has not been connected */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcConnection, rcInvalid );
        break;
    case ENOTDIR: /* component of path is not a directory */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcDirEntry, rcError );
        break;
    case ENOTSOCK: /* sock fd does not refer to socket */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcId, rcInvalid );
        break;
    case EOPNOTSUPP: /* bits in flags argument is inappropriate */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcParam, rcInvalid );
        break;
    case EPERM:
        rc = RC ( rcNS, rcNoTarg, rcReading, rcMemory, rcUnauthorized );            
        break;
    case EPIPE: /* local end has been shut down. Will also receive SIGPIPE or MSG_NOSIGNAL */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcConnection, rcCanceled );
        break;
    case EPROTONOSUPPORT: /* specified protocol is not supported */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcNoObj, rcError );
        break;
    case EROFS: /* socket inode on read only file system */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcNoObj, rcReadonly );
        break;
    case ETIMEDOUT: /* timeout */
        rc = RC ( rcNS, rcNoTarg, rcReading, rcConnection, rcNotAvailable );
        break;
#if ! defined EAGAIN || ! defined EWOULDBLOCK || EAGAIN != EWOULDBLOCK
    case EWOULDBLOCK:
        rc = RC ( rcNS, rcNoTarg, rcReading, rcId, rcError );
        break;
#endif
    default:
        rc = RC ( rcNS, rcNoTarg, rcReading, rcNoObj, rcError );
        PLOGERR (klogErr,
                 (klogErr, rc, "unknown system error '$(S)($(E))'",
                  "S=%!,E=%d", lerrno, lerrno));
    }
    
#if _DEBUGGING
    if ( rc != 0 )
        pLogMsg ( klogInfo, "$(RC)\n", "RC=%R", rc );
#endif

    return rc;
}

static
rc_t CC KSocketTimedRead ( const KSocket *self,
    void *buffer, size_t bsize, size_t *num_read, timeout_t *tm )
{
    int revents;
    
    assert ( self != NULL );
    assert ( num_read != NULL );

    pLogLibMsg(klogInfo, "$(b): KSocketTimedRead($(s), $(t))...", "b=%p,s=%d,t=%d", self, bsize, tm == NULL ? -1 : tm -> mS);
    
    /* wait for socket to become readable */
    revents = socket_wait ( self -> fd
                            , POLLIN
                            | POLLRDNORM
                            | POLLRDBAND
                            | POLLPRI
                            | POLLRDHUP
                            , tm );

    /* check for error */
    if ( revents < 0 || ( revents & ( POLLERR | POLLNVAL ) ) != 0 )
    {
        if ( errno != 0 )
        {
            rc_t rc = HandleErrno ( __func__, __LINE__ );
            pLogLibMsg(klogInfo, "$(b): KSocketTimedRead socket_wait "
                "returned errno $(e)", "b=%p,e=%d", self, errno);
            return rc;
        }

        if ((revents & POLLERR) != 0) {
            int optval = 0;
            socklen_t optlen = sizeof optval;
            if ((getsockopt(self->fd, SOL_SOCKET, SO_ERROR, &optval, &optlen)
                    == 0)
                && optval > 0)
            {
                errno = optval;
                DBGMSG(DBG_KNS, DBG_FLAG(DBG_KNS_ERR), (
"@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@1 %s getsockopt = %s @@@@@@@@@@@@@@@@"
                    "\n", __FILE__, strerror(optval)));
                rc_t rc = HandleErrno(__func__, __LINE__);
                pLogLibMsg(klogInfo, "$(b): KSocketTimedRead "
                    "socket_wait/getsockopt returned errno $(e)",
                    "b=%p,e=%d", self, errno);
                return rc;
            }
        }

        pLogLibMsg(klogInfo, "$(b): KSocketTimedRead socket_wait "
            "returned POLLERR | POLLNVAL", "b=%p", self);
        return RC ( rcNS, rcStream, rcReading, rcNoObj, rcUnknown );
    }

    /* check for read availability */
    if ( ( revents & ( POLLRDNORM | POLLRDBAND ) ) != 0 )
    {
        ssize_t count = recv ( self -> fd, buffer, bsize, 0 );
        if ( count >= 0 )
        {
            * num_read = count;
            return 0;
        }
        rc_t rc = HandleErrno ( __func__, __LINE__ );
        pLogLibMsg(klogInfo, "$(b): KSocketTimedRead recv returned count $(c)", "b=%p,c=%d", self, count);
        return rc;
    }

    /* check for broken connection */
    if ( ( revents & ( POLLHUP | POLLRDHUP ) ) != 0 )
    {
        pLogLibMsg(klogInfo, "$(b): KSocketTimedRead broken connection", "b=%p", self);
        * num_read = 0;
        return 0;
    }

    /* anything else in revents is an error */
    if ( ( revents & ~ POLLIN ) != 0 && errno != 0 )
    {
        rc_t rc = HandleErrno ( __func__, __LINE__ );
        pLogLibMsg(klogInfo, "$(b): KSocketTimedRead error=$(e)", "b=%p,e=%e", self, errno);
        return rc;
    }

    /* finally, call this a timeout */
    pLogLibMsg(klogInfo, "$(b): KSocketTimedRead timeout", "b=%p", self);
    return RC ( rcNS, rcStream, rcReading, rcTimeout, rcExhausted );
}

static
rc_t CC KSocketRead ( const KSocket *self,
    void *buffer, size_t bsize, size_t *num_read )
{
    timeout_t tm;
    assert ( self != NULL );

    if ( self -> read_timeout < 0 )
        return KSocketTimedRead ( self, buffer, bsize, num_read, NULL );

    TimeoutInit ( & tm, self -> read_timeout );
    return KSocketTimedRead ( self, buffer, bsize, num_read, & tm );
}

static
rc_t CC KSocketTimedWrite ( KSocket *self,
    const void *buffer, size_t bsize, size_t *num_writ, timeout_t *tm )
{
    int revents;
    ssize_t count;

    assert ( self != NULL );
    assert ( num_writ != NULL );

    pLogLibMsg(klogInfo, "$(b): KSocketTimedWrite($(s), $(t))...", "b=%p,s=%d,t=%d", self, bsize, tm == NULL ? -1 : tm -> mS);

    /* wait for socket to become writable */
    revents = socket_wait ( self -> fd
                            , POLLOUT
                            | POLLWRNORM
                            | POLLWRBAND
                            , tm );

    /* check for error */
    if ( revents < 0 || ( revents & ( POLLERR | POLLNVAL ) ) != 0 )
    {
        if ( errno != 0 )
        {
            rc_t rc = HandleErrno ( __func__, __LINE__ );
            pLogLibMsg(klogInfo, "$(b): KSocketTimedWrite socket_wait returned errno $(e)", "b=%p,e=%d", self, errno);
            return rc;
        }
        pLogLibMsg(klogInfo, "$(b): KSocketTimedWrite socket_wait returned POLLERR | POLLNVAL", "b=%p", self);
        return RC ( rcNS, rcStream, rcWriting, rcNoObj, rcUnknown );
    }

    /* check for broken connection */
    if ( ( revents & POLLHUP ) != 0 )
    {
        pLogLibMsg(klogInfo, "$(b): POLLHUP received", "b=%p", self);            
        * num_writ = 0;
        return 0;
    }

    /* check for ability to send */
    if ( ( revents & ( POLLWRNORM | POLLWRBAND ) ) != 0 )
    {
        rc_t rc = 0;
        count = send ( self -> fd, buffer, bsize, 0 );
        if ( count >= 0 )
        {
            pLogLibMsg(klogInfo, "$(b): $(s) bytes written", "b=%p,s=%d", self, count);            
            * num_writ = count;
            return 0;
        }

        rc = HandleErrno ( __func__, __LINE__ );
        pLogLibMsg(klogInfo, "$(b): KSocketTimedWrite recv returned count $(c)", "b=%p,c=%d", self, count);
        return rc;
    }

    /* anything else in revents is an error */
    if ( ( revents & ~ POLLOUT ) != 0 && errno != 0 )
    {
        rc_t rc = HandleErrno ( __func__, __LINE__ );
        pLogLibMsg(klogInfo, "$(b): KSocketTimedWrite error=$(e)", "b=%p,e=%e", self, errno);
        return rc;
    }

    /* finally, call this a timeout */
    pLogLibMsg(klogInfo, "$(b): KSocketTimedWrite timeout", "b=%p", self);            
    return RC ( rcNS, rcStream, rcWriting, rcTimeout, rcExhausted );
}

static
rc_t CC KSocketWrite ( KSocket *self,
    const void *buffer, size_t bsize, size_t *num_writ )
{
    timeout_t tm;
    assert ( self != NULL );

    if ( self -> write_timeout < 0 )
        return KSocketTimedWrite ( self, buffer, bsize, num_writ, NULL );

    TimeoutInit ( & tm, self -> write_timeout );
    return KSocketTimedWrite ( self, buffer, bsize, num_writ, & tm );
}

static KStream_vt_v1 vtKSocket =
{
    1, 1,
    KSocketWhack,
    KSocketRead,
    KSocketWrite,
    KSocketTimedRead,
    KSocketTimedWrite
};

static
rc_t KSocketMakePath ( const char * name, char * buf, size_t buf_size )
{
    size_t num_writ;
#if 0
    struct passwd* pwd;
    pwd = getpwuid ( geteuid () );
    if ( pwd == NULL )
        return HandleErrno ( __func__, __LINE__ );

    return string_printf ( buf, buf_size, & num_writ, "%s/.ncbi/%s", pwd -> pw_dir, name );
#else
    const char *HOME = getenv ( "HOME" );
    if ( HOME == NULL )
        return RC ( rcNS, rcProcess, rcAccessing, rcPath, rcNotFound );

    return string_printf ( buf, buf_size, & num_writ, "%s/.ncbi/%s", HOME, name );
#endif
}

static
rc_t KSocketConnectIPv4 ( KSocket *self, int32_t retryTimeout, const KEndPoint *from, const KEndPoint *to )
{
    rc_t rc = 0;
    uint32_t retry_count = 0;
    struct sockaddr_in ss_from, ss_to;

    memset ( & ss_from, 0, sizeof ss_from );
    if ( from != NULL )
    {
        ss_from . sin_family = AF_INET;
        ss_from . sin_addr . s_addr = htonl ( from -> u . ipv4 . addr );
        ss_from . sin_port = htons ( from -> u . ipv4 . port );
    }

    memset ( & ss_to, 0, sizeof ss_to );
    ss_to . sin_family = AF_INET;
    ss_to . sin_addr . s_addr = htonl ( to -> u . ipv4 . addr );
    ss_to . sin_port = htons ( to -> u . ipv4 . port );

    do 
    {
        /* create the OS socket */
        self -> fd = socket ( AF_INET, SOCK_STREAM, 0 );
        if ( self -> fd < 0 )
            rc = HandleErrno ( __func__, __LINE__ );
        else
        {
            /* disable nagle algorithm */
            int flag = 1;
            setsockopt ( self -> fd, IPPROTO_TCP, TCP_NODELAY, ( char* ) & flag, sizeof flag );

            /* bind */
            if ( from != NULL && bind ( self -> fd, ( struct sockaddr* ) & ss_from, sizeof ss_from ) != 0 )
                rc = HandleErrno ( __func__, __LINE__ );
                
            if ( rc == 0 )
            {
                /* connect */
                if ( connect ( self -> fd, ( struct sockaddr* ) & ss_to, sizeof ss_to ) == 0 )
                {
                    /* set non-blocking mode */
                    flag = fcntl ( self -> fd, F_GETFL );
                    fcntl ( self -> fd, F_SETFL, flag | O_NONBLOCK );
                    return 0;
                }
                rc = HandleErrno ( __func__, __LINE__ );
            }

            /* dump socket */
            close ( self -> fd );
            self -> fd = -1;
        }
        
        /* rc != 0 */
        if (retryTimeout < 0 || retry_count < retryTimeout)
        {   /* retry */
            sleep ( 1 );
            ++retry_count;
            rc = 0;
        }
    }
    while (rc == 0);
    
    pLogLibMsg(klogInfo, "$(b): KSocketConnectIPv4 timed out", "b=%p", self);            

    return rc;
}

static
rc_t KSocketConnectIPC ( KSocket *self, int32_t retryTimeout, const KEndPoint *to )
{
    rc_t rc = 0;
    uint32_t retry_count = 0;
    struct sockaddr_un ss_to;

    memset ( & ss_to, 0, sizeof ss_to );
    ss_to . sun_family = AF_UNIX;
    rc = KSocketMakePath ( to -> u . ipc_name, ss_to . sun_path, sizeof ss_to . sun_path );

    do 
    {
        /* create the OS socket */
        self -> fd = socket ( AF_UNIX, SOCK_STREAM, 0 );
        if ( self -> fd < 0 )
            rc = HandleErrno ( __func__, __LINE__ );
        else
        {
            /* connect */
            if ( connect ( self -> fd, ( struct sockaddr* ) & ss_to, sizeof ss_to ) == 0 )
            {
                return 0;
            }
            rc = HandleErrno ( __func__, __LINE__ );

            /* dump socket */
            close ( self -> fd );
            self -> fd = -1;
        }
        
        /* rc != 0 */
        if (retryTimeout < 0 || retry_count < retryTimeout)
        {   /* retry */
            sleep ( 1 );
            ++retry_count;
            rc = 0;
        }
    }
    while (rc == 0);

    pLogLibMsg(klogInfo, "$(b): KSocketConnectIPC timed out", "b=%p", self);            

    return rc;
 }

KNS_EXTERN rc_t CC KNSManagerMakeRetryTimedConnection ( struct KNSManager const * self,
    struct KStream **out, int32_t retryTimeout, int32_t readMillis, int32_t writeMillis,
    struct KEndPoint const *from, struct KEndPoint const *to )
{
    rc_t rc;

    if ( out == NULL )
        rc = RC ( rcNS, rcStream, rcConstructing, rcParam, rcNull );
    else
    {
        if ( self == NULL )
            rc = RC ( rcNS, rcStream, rcConstructing, rcSelf, rcNull );
        else if ( to == NULL )
            rc = RC ( rcNS, rcStream, rcConstructing, rcParam, rcNull );
        else if ( from != NULL && from -> type != to -> type )
            rc = RC ( rcNS, rcStream, rcConstructing, rcParam, rcIncorrect );
        else
        {
            KSocket *conn = calloc ( 1, sizeof * conn );
            if ( conn == NULL )
                rc = RC ( rcNS, rcStream, rcConstructing, rcMemory, rcExhausted );
            else
            {
                conn -> fd = -1;
                conn -> read_timeout = readMillis;
                conn -> write_timeout = writeMillis;

                rc = KStreamInit ( & conn -> dad, ( const KStream_vt* ) & vtKSocket,
                                   "KSocket", "", true, true );
                if ( rc == 0 )
                {
                    switch ( to -> type )
                    {
                    case epIPV4:
                        rc = KSocketConnectIPv4 ( conn, retryTimeout, from, to );
                        break;
                    case epIPC:
                        rc = KSocketConnectIPC ( conn, retryTimeout, to );
                        break;
                    default:
                        rc = RC ( rcNS, rcStream, rcConstructing, rcParam, rcIncorrect );
                    }

                    if ( rc == 0 )
                    {
                        * out = & conn -> dad;
                        return 0;
                    }
                }

                free ( conn );
            }
        }

        * out = NULL;
    }

    return rc;
}

static
rc_t KNSManagerMakeIPv4Listener ( KSocket *listener, const KEndPoint * ep )
{
    rc_t rc;

    listener -> fd = socket ( AF_INET, SOCK_STREAM, 0 );
    if ( listener -> fd < 0 )
        rc = HandleErrno ( __func__, __LINE__ );
    else
    {
        struct sockaddr_in ss;

        int on = 1;
        setsockopt ( listener -> fd, SOL_SOCKET, SO_REUSEADDR, ( char* ) & on, sizeof on );

        memset ( & ss, 0, sizeof ss );
        ss . sin_family = AF_INET;
        ss . sin_addr . s_addr = htonl ( ep -> u . ipv4 . addr );
        ss . sin_port = htons ( ep -> u . ipv4 . port );

        if ( bind ( listener -> fd, ( struct sockaddr* ) & ss, sizeof ss ) == 0 )
            return 0;
        rc = HandleErrno ( __func__, __LINE__ );

        close ( listener -> fd );
        listener -> fd = -1;
    }

    return rc;
}

static
rc_t KNSManagerMakeIPCListener ( KSocket *listener, const KEndPoint * ep )
{
    rc_t rc;

    listener -> fd = socket ( AF_UNIX, SOCK_STREAM, 0 );
    if ( listener -> fd < 0 )
        rc = HandleErrno ( __func__, __LINE__ );
    else
    {
        struct sockaddr_un ss;
        memset ( & ss, 0, sizeof ss );
        ss.sun_family = AF_UNIX;
        rc = KSocketMakePath ( ep -> u. ipc_name, ss . sun_path, sizeof ss . sun_path );
        if ( rc == 0 )
        {
            char * path = string_dup ( ss . sun_path, string_measure ( ss . sun_path, NULL ) );
            if ( path == NULL )
                rc = RC ( rcNS, rcSocket, rcConstructing, rcMemory, rcExhausted );
            else
            {
                unlink ( ss . sun_path );
                if ( bind ( listener -> fd, ( struct sockaddr* ) & ss, sizeof ss ) != 0 )
                    rc = HandleErrno ( __func__, __LINE__ );
                else
                {
                    listener -> path = path;
                    return 0;
                }

                free ( path );
            }
        }

        close ( listener -> fd );
        listener -> fd = -1;
    }

    return rc;
}

LIB_EXPORT rc_t CC KNSManagerMakeListener ( const KNSManager *self,
    KSocket ** out, const KEndPoint * ep )
{   
    rc_t rc;

    if ( out == NULL )
        rc = RC ( rcNS, rcSocket, rcConstructing, rcParam, rcNull );
    else
    {
        if ( self == NULL )
            rc = RC ( rcNS, rcSocket, rcConstructing, rcSelf, rcNull );
        else if ( ep == NULL )
            rc = RC ( rcNS, rcSocket, rcConstructing, rcParam, rcNull );
        else
        {
            KSocket *listener = calloc ( 1, sizeof * listener );
            if ( listener == NULL )
                rc = RC ( rcNS, rcSocket, rcConstructing, rcMemory, rcExhausted );
            else
            {
                listener -> fd = -1;

                /* pass these along to accepted sockets */
                listener -> read_timeout = self -> conn_read_timeout;
                listener -> write_timeout = self -> conn_write_timeout;

                rc = KStreamInit ( & listener -> dad, ( const KStream_vt* ) & vtKSocket,
                                   "KSocket", "", true, true );
                if ( rc == 0 )
                {
                    switch ( ep -> type )
                    {
                    case epIPV4:
                        rc = KNSManagerMakeIPv4Listener ( listener, ep );
                        break;
                    case epIPC:
                        rc = KNSManagerMakeIPCListener ( listener, ep );
                        break;
                    default:
                        rc = RC ( rcNS, rcSocket, rcConstructing, rcParam, rcIncorrect );
                    }

                    if ( rc == 0 )
                    {
                        /* the classic 5 connection queue... ? */
                        if ( listen ( listener -> fd, 5 ) == 0 )
                        {
                            * out = listener;
                            return 0;
                        }

                        rc = HandleErrno ( __func__, __LINE__ );

                        if ( listener -> path != NULL )
                            free ( ( void* ) listener -> path );
                    }
                }

                free ( listener );
            }
        }

        * out = NULL;
    }

    return rc;
}

static
rc_t KSocketAcceptIPv4 ( KSocket *self, KSocket *conn )
{
    struct sockaddr_in remote;
    socklen_t len = sizeof remote;
    conn -> fd = accept ( self -> fd, ( struct sockaddr* ) & remote, & len );
    if ( conn -> fd < 0 )
        return HandleErrno ( __func__, __LINE__ );
    if ( len > sizeof remote )
        return RC ( rcNS, rcConnection, rcWaiting, rcBuffer, rcInsufficient );
    return 0;
}

static
rc_t KSocketAcceptIPC ( KSocket *self, KSocket *conn )
{
    struct sockaddr_un remote;
    socklen_t len = sizeof remote;
    conn -> fd = accept ( self -> fd, ( struct sockaddr* ) & remote, & len );
    if ( conn -> fd < 0 )
        return HandleErrno ( __func__, __LINE__ );
    if ( len > sizeof remote )
        return RC ( rcNS, rcConnection, rcWaiting, rcBuffer, rcInsufficient );
    return 0;
}

LIB_EXPORT rc_t CC KSocketAccept ( KSocket *self, struct KStream **out )
{
    rc_t rc;

    if ( out == NULL )
        rc = RC ( rcNS, rcConnection, rcWaiting, rcParam, rcNull );
    else
    {
        if ( self == NULL )
            rc = RC ( rcNS, rcConnection, rcWaiting, rcSelf, rcNull);
        else
        {
            KSocket * conn = calloc ( 1, sizeof * conn );
            if ( conn == NULL )
                rc = RC ( rcNS, rcConnection, rcWaiting, rcMemory, rcExhausted );
            else
            {
                conn -> fd = -1;
                conn -> read_timeout = self -> read_timeout;
                conn -> write_timeout = self -> write_timeout;

                rc = KStreamInit ( & conn -> dad, ( const KStream_vt* ) & vtKSocket,
                                   "KSocket", "", true, true );
                if ( rc == 0 )
                {
                    switch ( self -> type )
                    {
                    case epIPV4:
                        rc = KSocketAcceptIPv4 ( self, conn );
                        break;
                    case epIPC:
                        rc = KSocketAcceptIPC ( self, conn );
                        break;
                    default:
                        rc = RC ( rcNS, rcSocket, rcConstructing, rcSelf, rcCorrupt );
                    }

                    if ( rc == 0 )
                    {
                        * out = & conn -> dad;
                        return 0;
                    }

                    free ( conn );
                }
            }
        }

        * out = NULL;
    }

    return rc;
}

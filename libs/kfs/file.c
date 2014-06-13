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

#include <kfs/extern.h>
#include <kfs/impl.h>
#include <klib/rc.h>
#include <kproc/timeout.h>
#include <os-native.h>
#include <sysalloc.h>

#include <assert.h>

/*--------------------------------------------------------------------------
 * KFile
 *  a file
 */

/* Destroy
 *  destroy file
 */
LIB_EXPORT rc_t CC KFileDestroy ( KFile *self )
{
    if ( self == NULL )
        return RC ( rcFS, rcFile, rcDestroying, rcSelf, rcNull );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        return ( * self -> vt -> v1 . destroy ) ( self );
    }

    return RC ( rcFS, rcFile, rcDestroying, rcInterface, rcBadVersion );
}

/* GetSysFile
 *  returns an underlying system file object
 *  and starting offset to contiguous region
 *  suitable for memory mapping, or NULL if
 *  no such file is available.
 */
LIB_EXPORT struct KSysFile* CC KFileGetSysFile ( const KFile *self, uint64_t *offset )
{
    if ( offset != NULL )
    {
        * offset = 0;
        if ( self != NULL )
        {
            switch ( self -> vt -> v1 . maj )
            {
            case 1:
                return ( * self -> vt -> v1 . get_sysfile ) ( self, offset );
            }
        }
    }
    return NULL;
}

/* AddRef
 *  creates a new reference
 *  ignores NULL references
 */
LIB_EXPORT rc_t CC KFileAddRef ( const KFile *self )
{
    if ( self != NULL )
    {
        switch ( KRefcountAdd ( & self -> refcount, "KFile" ) )
        {
        case krefLimit:
            return RC ( rcFS, rcFile, rcAttaching, rcRange, rcExcessive );
        case krefNegative:
            return RC ( rcFS, rcFile, rcAttaching, rcSelf, rcInvalid );
        default:
            break;
        }
    }
    return 0;
}

/* Release
 *  discard reference to file
 *  ignores NULL references
 */
LIB_EXPORT rc_t CC KFileRelease ( const KFile *self )
{
    if ( self != NULL )
    {
        switch ( KRefcountDrop ( & self -> refcount, "KFile" ) )
        {
        case krefWhack:
            if ( self -> dir != NULL )
                return KDirectoryDestroyFile ( self -> dir, ( KFile* ) self );
            return KFileDestroy ( ( KFile* ) self );
        case krefNegative:
            return RC ( rcFS, rcFile, rcReleasing, rcRange, rcExcessive );
        default:
            break;
        }
    }

    return 0;
}

/* RandomAccess
 *  ALMOST by definition, the file is random access
 *  certain file types ( notably compressors ) will refuse random access
 *
 *  returns 0 if random access, error code otherwise
 */
LIB_EXPORT rc_t CC KFileRandomAccess ( const KFile *self )
{
    if ( self == NULL )
        return RC ( rcFS, rcFile, rcAccessing, rcSelf, rcNull );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        return ( * self -> vt -> v1 . random_access ) ( self );
    }

    return RC ( rcFS, rcFile, rcAccessing, rcInterface, rcBadVersion );
}

/* Type
 *  returns a KFileDesc
 *  not intended to be a content type,
 *  but rather an implementation class
 */
LIB_EXPORT uint32_t CC KFileType ( const KFile *self )
{
    if ( self == NULL )
        return kfdNull;

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        if ( self -> vt -> v1 . min >= 1 )
            return ( * self -> vt -> v1 . get_type ) ( self );
        break;
    }

    return kfdInvalid;
}

/* Size
 *  returns size in bytes of file
 *
 *  "size" [ OUT ] - return parameter for file size
 */
LIB_EXPORT rc_t CC KFileSize ( const KFile *self, uint64_t *size )
{
    if ( size == NULL )
        return RC ( rcFS, rcFile, rcAccessing, rcParam, rcNull );

    * size = 0;

    if ( self == NULL )
        return RC ( rcFS, rcFile, rcAccessing, rcSelf, rcNull );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        return ( * self -> vt -> v1 . get_size ) ( self, size );
    }

    return RC ( rcFS, rcFile, rcAccessing, rcInterface, rcBadVersion );
}

/* SetSize
 *  sets size in bytes of file
 *
 *  "size" [ IN ] - new file size
 */
LIB_EXPORT rc_t CC KFileSetSize ( KFile *self, uint64_t size )
{
    if ( self == NULL )
        return RC ( rcFS, rcFile, rcResizing, rcSelf, rcNull );

    if ( ! self -> write_enabled )
        return RC ( rcFS, rcFile, rcResizing, rcFile, rcNoPerm );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        return ( * self -> vt -> v1 . set_size ) ( self, size );
    }

    return RC ( rcFS, rcFile, rcResizing, rcInterface, rcBadVersion );
}

/* Read
 *  read file from known position
 *
 *  "pos" [ IN ] - starting position within file
 *
 *  "buffer" [ OUT ] and "bsize" [ IN ] - return buffer for read
 *
 *  "num_read" [ OUT ] - number of bytes actually read
 */
LIB_EXPORT rc_t CC KFileRead ( const KFile *self, uint64_t pos,
    void *buffer, size_t bsize, size_t *num_read )
{
    if ( num_read == NULL )
        return RC ( rcFS, rcFile, rcReading, rcParam, rcNull );

    * num_read = 0;

    if ( self == NULL )
        return RC ( rcFS, rcFile, rcReading, rcSelf, rcNull );

    if ( ! self -> read_enabled )
        return RC ( rcFS, rcFile, rcReading, rcFile, rcNoPerm );

    if ( buffer == NULL )
        return RC ( rcFS, rcFile, rcReading, rcBuffer, rcNull );
    if ( bsize == 0 )
        return RC ( rcFS, rcFile, rcReading, rcBuffer, rcInsufficient );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        return ( * self -> vt -> v1 . read ) ( self, pos, buffer, bsize, num_read );
    }

    return RC ( rcFS, rcFile, rcReading, rcInterface, rcBadVersion );
}

LIB_EXPORT rc_t CC KFileTimedRead ( const KFile *self, uint64_t pos,
    void *buffer, size_t bsize, size_t *num_read, struct timeout_t *tm )
{
    if ( num_read == NULL )
        return RC ( rcFS, rcFile, rcReading, rcParam, rcNull );

    * num_read = 0;

    if ( self == NULL )
        return RC ( rcFS, rcFile, rcReading, rcSelf, rcNull );

    if ( ! self -> read_enabled )
        return RC ( rcFS, rcFile, rcReading, rcFile, rcNoPerm );

    if ( buffer == NULL )
        return RC ( rcFS, rcFile, rcReading, rcBuffer, rcNull );
    if ( bsize == 0 )
        return RC ( rcFS, rcFile, rcReading, rcBuffer, rcInsufficient );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        if ( self -> vt -> v1 . min >= 2 )
            return ( * self -> vt -> v1 . timed_read ) ( self, pos, buffer, bsize, num_read, tm );
        if ( tm == NULL )
            return ( * self -> vt -> v1 . read ) ( self, pos, buffer, bsize, num_read );
        break;
    }

    return RC ( rcFS, rcFile, rcReading, rcInterface, rcBadVersion );
}

/* ReadAll
 *  read from file until "bsize" bytes have been retrieved
 *  or until end-of-input
 *
 *  "pos" [ IN ] - starting position within file
 *
 *  "buffer" [ OUT ] and "bsize" [ IN ] - return buffer for read
 *
 *  "num_read" [ OUT ] - return parameter giving number of bytes
 *  actually read. when returned value is zero and return code is
 *  also zero, interpreted as end of file.
 */
LIB_EXPORT rc_t CC KFileReadAll ( const KFile *self, uint64_t pos,
    void *buffer, size_t bsize, size_t *num_read )
{
    rc_t rc;
    uint8_t *b;
    size_t total, count;

    if ( num_read == NULL )
        return RC ( rcFS, rcFile, rcReading, rcParam, rcNull );

    * num_read = 0;

    if ( self == NULL )
        return RC ( rcFS, rcFile, rcReading, rcSelf, rcNull );

    if ( ! self -> read_enabled )
        return RC ( rcFS, rcFile, rcReading, rcFile, rcNoPerm );

    if ( buffer == NULL )
        return RC ( rcFS, rcFile, rcReading, rcBuffer, rcNull );
    if ( bsize == 0 )
        return RC ( rcFS, rcFile, rcReading, rcBuffer, rcInsufficient );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        count = 0;
        rc = ( * self -> vt -> v1 . read ) ( self, pos, buffer, bsize, & count );
        total = count;

        if ( rc == 0 && count != 0 && count < bsize )
        {
            if ( self -> vt -> v1 . min >= 2 )
            {
                timeout_t no_block;
                TimeoutInit ( & no_block, 0 );

                for ( b = buffer; total < bsize; total += count )
                {
                    count = 0;
                    rc = ( * self -> vt -> v1 . timed_read ) ( self, pos + total, b + total, bsize - total, & count, & no_block );
                    if ( rc != 0 )
                        break;
                    if ( count == 0 )
                        break;
                }
            }
            else
            {
                for ( b = buffer; total < bsize; total += count )
                {
                    count = 0;
                    rc = ( * self -> vt -> v1 . read ) ( self, pos + total, b + total, bsize - total, & count );
                    if ( rc != 0 )
                        break;
                    if ( count == 0 )
                        break;
                }
            }
        }
        break;
    default:
        return RC ( rcFS, rcFile, rcReading, rcInterface, rcBadVersion );
    }

    if ( total != 0 )
    {
        * num_read = total;
        return 0;
    }

    return rc;
}

LIB_EXPORT rc_t CC KFileTimedReadAll ( const KFile *self, uint64_t pos,
    void *buffer, size_t bsize, size_t *num_read, struct timeout_t *tm )
{
    rc_t rc;
    uint8_t *b;
    size_t total, count;

    if ( num_read == NULL )
        return RC ( rcFS, rcFile, rcReading, rcParam, rcNull );

    * num_read = 0;

    if ( self == NULL )
        return RC ( rcFS, rcFile, rcReading, rcSelf, rcNull );

    if ( ! self -> read_enabled )
        return RC ( rcFS, rcFile, rcReading, rcFile, rcNoPerm );

    if ( buffer == NULL )
        return RC ( rcFS, rcFile, rcReading, rcBuffer, rcNull );
    if ( bsize == 0 )
        return RC ( rcFS, rcFile, rcReading, rcBuffer, rcInsufficient );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        if ( self -> vt -> v1 . min >= 2 )
        {
            count = 0;
            rc = ( * self -> vt -> v1 . timed_read ) ( self, pos, buffer, bsize, & count, tm );
            total = count;

            if ( rc == 0 && count != 0 && count < bsize )
            {
                timeout_t no_block;
                TimeoutInit ( & no_block, 0 );

                for ( b = buffer; total < bsize; total += count )
                {
                    count = 0;
                    rc = ( * self -> vt -> v1 . timed_read ) ( self, pos + total, b + total, bsize - total, & count, & no_block );
                    if ( rc != 0 )
                        break;
                    if ( count == 0 )
                        break;
                }
            }
            break;
        }

        if ( tm == NULL )
        {
            for ( rc = 0, b = buffer, total = 0; total < bsize; total += count )
            {
                count = 0;
                rc = ( * self -> vt -> v1 . read ) ( self, pos + total, b + total, bsize - total, & count );
                if ( rc != 0 )
                    break;
                if ( count == 0 )
                    break;
            }
            break;
        }

        /* no break */
    default:
        return RC ( rcFS, rcFile, rcReading, rcInterface, rcBadVersion );
    }

    if ( total != 0 )
    {
        * num_read = total;
        return 0;
    }

    return rc;
}

/* ReadExactly
 * TimedReadExactly
 *  read from file until "bytes" have been retrieved
 *  or return incomplete transfer error
 *
 *  "pos" [ IN ] - starting position within file
 *
 *  "buffer" [ OUT ] and "bytes" [ IN ] - return buffer for read
 *
 *  "tm" [ IN/OUT, NULL OKAY ] - an optional indicator of
 *  blocking behavior. not all implementations will support
 *  timed reads. a NULL timeout will block indefinitely,
 *  a value of "tm->mS == 0" will have non-blocking behavior
 *  if supported by implementation, and "tm->mS > 0" will indicate
 *  a maximum wait timeout.
 */
LIB_EXPORT rc_t CC KFileReadExactly ( const KFile *self,
    uint64_t pos, void *buffer, size_t bytes )
{
    rc_t rc;
    uint8_t *b;
    size_t total, count;

    if ( self == NULL )
        return RC ( rcFS, rcFile, rcReading, rcSelf, rcNull );

    if ( ! self -> read_enabled )
        return RC ( rcFS, rcFile, rcReading, rcFile, rcNoPerm );

    if ( bytes == 0 )
        return 0;
    if ( buffer == NULL )
        return RC ( rcFS, rcFile, rcReading, rcBuffer, rcNull );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        assert ( bytes != 0 );
        for ( b = buffer, total = 0; total < bytes; total += count )
        {
            count = 0;
            rc = ( * self -> vt -> v1 . read ) ( self, pos + total, b + total, bytes - total, & count );
            if ( rc != 0 )
            {
                if ( GetRCObject ( rc ) != rcTimeout || GetRCState ( rc ) != rcExhausted )
                    break;
            }
            else if ( count == 0 )
            {
                rc = RC ( rcFS, rcFile, rcReading, rcTransfer, rcIncomplete );
                break;
            }
        }
        break;
    default:
        rc = RC ( rcFS, rcFile, rcReading, rcInterface, rcBadVersion );
    }

    return rc;
}

LIB_EXPORT rc_t CC KFileTimedReadExactly ( const KFile *self,
    uint64_t pos, void *buffer, size_t bytes, struct timeout_t *tm )
{
    rc_t rc;
    uint8_t *b;
    size_t total, count;

    if ( self == NULL )
        return RC ( rcFS, rcFile, rcReading, rcSelf, rcNull );

    if ( ! self -> read_enabled )
        return RC ( rcFS, rcFile, rcReading, rcFile, rcNoPerm );

    if ( bytes == 0 )
        return 0;
    if ( buffer == NULL )
        return RC ( rcFS, rcFile, rcReading, rcBuffer, rcNull );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        if ( self -> vt -> v1 . min >= 2 )
        {
            assert ( bytes != 0 );
            for ( b = buffer, total = 0; total < bytes; total += count )
            {
                count = 0;
                rc = ( * self -> vt -> v1 . timed_read ) ( self, pos + total, b + total, bytes - total, & count, tm );
                if ( rc != 0 )
                {
                    if ( tm != NULL )
                        break;
                    if ( GetRCObject ( rc ) != rcTimeout || GetRCState ( rc ) != rcExhausted )
                        break;
                }
                else if ( count == 0 )
                {
                    rc = RC ( rcFS, rcFile, rcReading, rcTransfer, rcIncomplete );
                    break;
                }
            }
            break;
        }

        if ( tm == NULL )
        {
            assert ( bytes != 0 );
            for ( b = buffer, total = 0; total < bytes; total += count )
            {
                count = 0;
                rc = ( * self -> vt -> v1 . read ) ( self, pos + total, b + total, bytes - total, & count );
                if ( rc != 0 )
                {
                    if ( GetRCObject ( rc ) != rcTimeout || GetRCState ( rc ) != rcExhausted )
                        break;
                }
                else if ( count == 0 )
                {
                    rc = RC ( rcFS, rcFile, rcReading, rcTransfer, rcIncomplete );
                    break;
                }
            }
            break;
        }

        /* no break */
    default:
        return RC ( rcFS, rcFile, rcReading, rcInterface, rcBadVersion );
    }

    return rc;
}

/* Write
 *  write file at known position
 *
 *  "pos" [ IN ] - starting position within file
 *
 *  "buffer" [ IN ] and "size" [ IN ] - data to be written
 *
 *  "num_writ" [ OUT ] - number of bytes actually written
 */
LIB_EXPORT rc_t CC KFileWrite ( KFile *self, uint64_t pos,
    const void *buffer, size_t size, size_t *num_writ )
{
    size_t ignore;
    if ( num_writ == NULL )
        num_writ = & ignore;

    * num_writ = 0;

    if ( self == NULL )
        return RC ( rcFS, rcFile, rcWriting, rcSelf, rcNull );

    if ( ! self -> write_enabled )
        return RC ( rcFS, rcFile, rcWriting, rcFile, rcNoPerm );

    if ( size == 0 )
        return 0;
    if ( buffer == NULL )
        return RC ( rcFS, rcFile, rcWriting, rcBuffer, rcNull );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        return ( * self -> vt -> v1 . write ) ( self, pos, buffer, size, num_writ );
    }

    return RC ( rcFS, rcFile, rcWriting, rcInterface, rcBadVersion );
}

LIB_EXPORT rc_t CC KFileTimedWrite ( KFile *self, uint64_t pos,
    const void *buffer, size_t size, size_t *num_writ, struct timeout_t *tm )
{
    size_t ignore;
    if ( num_writ == NULL )
        num_writ = & ignore;

    * num_writ = 0;

    if ( self == NULL )
        return RC ( rcFS, rcFile, rcWriting, rcSelf, rcNull );

    if ( ! self -> write_enabled )
        return RC ( rcFS, rcFile, rcWriting, rcFile, rcNoPerm );

    if ( size == 0 )
        return 0;
    if ( buffer == NULL )
        return RC ( rcFS, rcFile, rcWriting, rcBuffer, rcNull );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        if ( self -> vt -> v1 . min >= 2 )
            return ( * self -> vt -> v1 . timed_write ) ( self, pos, buffer, size, num_writ, tm );
        if ( tm == NULL )
            return ( * self -> vt -> v1 . write ) ( self, pos, buffer, size, num_writ );
        break;
    }

    return RC ( rcFS, rcFile, rcWriting, rcInterface, rcBadVersion );
}

/* WriteAll
 *  write from file until "size" bytes have been transferred
 *  or until no further progress can be made
 *
 *  "pos" [ IN ] - starting position within file
 *
 *  "buffer" [ IN ] and "size" [ IN ] - data to be written
 *
 *  "num_writ" [ OUT, NULL OKAY ] - optional return parameter
 *  giving number of bytes actually written
 */
LIB_EXPORT rc_t CC KFileWriteAll ( KFile *self, uint64_t pos,
    const void *buffer, size_t size, size_t *num_writ )
{
    rc_t rc;
    const uint8_t *b;
    size_t total, count;

    size_t ignore;
    if ( num_writ == NULL )
        num_writ = & ignore;

    * num_writ = 0;

    if ( self == NULL )
        return RC ( rcFS, rcFile, rcWriting, rcSelf, rcNull );

    if ( ! self -> write_enabled )
        return RC ( rcFS, rcFile, rcWriting, rcFile, rcNoPerm );

    if ( size == 0 )
        return 0;
    if ( buffer == NULL )
        return RC ( rcFS, rcFile, rcWriting, rcBuffer, rcNull );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        count = 0;
        rc = ( * self -> vt -> v1 . write ) ( self, pos, buffer, size, & count );
        total = count;

        if ( rc == 0 && count != 0 && count < size )
        {
            if ( self -> vt -> v1 . min >= 2 )
            {
                timeout_t no_block;
                TimeoutInit ( & no_block, 0 );

                for ( b = buffer; total < size; total += count )
                {
                    count = 0;
                    rc = ( * self -> vt -> v1 . timed_write ) ( self, pos + total, b + total, size - total, & count, & no_block );
                    if ( rc != 0 )
                        break;
                    if ( count == 0 )
                        break;
                }
            }
            else
            {
                for ( b = buffer; total < size; total += count )
                {
                    count = 0;
                    rc = ( * self -> vt -> v1 . write ) ( self, pos + total, b + total, size - total, & count );
                    if ( rc != 0 )
                        break;
                    if ( count == 0 )
                        break;
                }
            }
        }
        break;
    default:
        return RC ( rcFS, rcFile, rcWriting, rcInterface, rcBadVersion );
    }

    * num_writ = total;
    if ( total == size )
        return 0;
    if ( rc == 0 )
        return RC ( rcFS, rcFile, rcWriting, rcTransfer, rcIncomplete );
    return rc;
}

LIB_EXPORT rc_t CC KFileTimedWriteAll ( KFile *self, uint64_t pos,
    const void *buffer, size_t size, size_t *num_writ, struct timeout_t *tm )
{
    rc_t rc;
    const uint8_t *b;
    size_t total, count;

    size_t ignore;
    if ( num_writ == NULL )
        num_writ = & ignore;

    * num_writ = 0;

    if ( self == NULL )
        return RC ( rcFS, rcFile, rcWriting, rcSelf, rcNull );

    if ( ! self -> write_enabled )
        return RC ( rcFS, rcFile, rcWriting, rcFile, rcNoPerm );

    if ( size == 0 )
        return 0;
    if ( buffer == NULL )
        return RC ( rcFS, rcFile, rcWriting, rcBuffer, rcNull );

    switch ( self -> vt -> v1 . maj )
    {
    case 1:
        if ( self -> vt -> v1 . min >= 2 )
        {
            for ( rc = 0, b = buffer, total = 0; total < size; total += count )
            {
                count = 0;
                rc = ( * self -> vt -> v1 . timed_write ) ( self, pos + total, b + total, size - total, & count, tm );
                if ( rc != 0 )
                    break;
                if ( count == 0 )
                    break;
            }
            break;
        }

        if ( tm == NULL )
        {
            for ( rc = 0, b = buffer, total = 0; total < size; total += count )
            {
                count = 0;
                rc = ( * self -> vt -> v1 . write ) ( self, pos + total, b + total, size - total, & count );
                if ( rc != 0 )
                    break;
                if ( count == 0 )
                    break;
            }
            break;
        }

        /* no break */

    default:
        return RC ( rcFS, rcFile, rcWriting, rcInterface, rcBadVersion );
    }

    * num_writ = total;
    if ( total == size )
        return 0;
    if ( rc == 0 )
        return RC ( rcFS, rcFile, rcWriting, rcTransfer, rcIncomplete );
    return rc;
}

/* Init
 *  initialize a newly allocated file object
 */
LIB_EXPORT rc_t CC KFileInit ( KFile *self, const KFile_vt *vt,
    const char *classname, const char *fname,
    bool read_enabled, bool write_enabled )
{
    if ( self == NULL )
        return RC ( rcFS, rcFile, rcConstructing, rcSelf, rcNull );
    if ( vt == NULL )
        return RC ( rcFS, rcFile, rcConstructing, rcInterface, rcNull );
    switch ( vt -> v1 . maj )
    {
    case 0:
        return RC ( rcFS, rcFile, rcConstructing, rcInterface, rcInvalid );

    case 1:
        switch ( vt -> v1 . min )
        {
            /* ADD NEW MINOR VERSION CASES HERE */
        case 2:
#if _DEBUGGING
            if ( vt -> v1 . timed_write == NULL ||
                 vt -> v1 . timed_read == NULL )
                return RC ( rcFS, rcFile, rcConstructing, rcInterface, rcNull );
#endif
        case 1:
#if _DEBUGGING
            if ( vt -> v1 . get_type == NULL )
                return RC ( rcFS, rcFile, rcConstructing, rcInterface, rcNull );
#endif
            /* no break */
        case 0:
#if _DEBUGGING
        if ( vt -> v1 . write == NULL         ||
             vt -> v1 . read == NULL          ||
             vt -> v1 . set_size == NULL      ||
             vt -> v1 . get_size == NULL      ||
             vt -> v1 . random_access == NULL ||
             vt -> v1 . get_sysfile == NULL   ||
             vt -> v1 . destroy == NULL )
        return RC ( rcFS, rcFile, rcConstructing, rcInterface, rcNull );
#endif
            break;
        default:
            return RC ( rcFS, rcFile, rcConstructing, rcInterface, rcBadVersion );
        }
        break;

    default:
        return RC ( rcFS, rcFile, rcConstructing, rcInterface, rcBadVersion );
    }

    self -> vt = vt;
    self -> dir = NULL;
    KRefcountInit ( & self -> refcount, 1, classname, "init", fname );
    self -> read_enabled = ( uint8_t ) ( read_enabled != 0 );
    self -> write_enabled = ( uint8_t ) ( write_enabled != 0 );

    return 0;
}

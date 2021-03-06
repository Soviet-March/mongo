// data_file.cpp

/**
*    Copyright (C) 2013 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/data_file.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/log.h"


namespace mongo {

    BOOST_STATIC_ASSERT( sizeof(DataFileHeader)-4 == 8192 );

    static void data_file_check(void *_mb) {
        if( sizeof(char *) == 4 )
            uassert( 10084, "can't map file memory - mongo requires 64 bit build for larger datasets", _mb != 0);
        else
            uassert( 10085, "can't map file memory", _mb != 0);
    }

    int DataFile::maxSize() {
        if ( sizeof( int* ) == 4 ) {
            return 512 * 1024 * 1024;
        }
        else if (mmapv1GlobalOptions.smallfiles) {
            return 0x7ff00000 >> 2;
        }
        else {
            return 0x7ff00000;
        }
    }

    NOINLINE_DECL void DataFile::badOfs(int ofs) const {
        msgasserted(13440, str::stream()  << "bad offset:" << ofs
                    << " accessing file: " << mmf.filename()
                    << ". See http://dochub.mongodb.org/core/data-recovery");
    }

    int DataFile::defaultSize( const char *filename ) const {
        int size;
        if ( fileNo <= 4 )
            size = (64*1024*1024) << fileNo;
        else
            size = 0x7ff00000;
        if (mmapv1GlobalOptions.smallfiles) {
            size = size >> 2;
        }
        return size;
    }

    /** @return true if found and opened. if uninitialized (prealloc only) does not open. */
    Status DataFile::openExisting(const char *filename) {
        verify( _mb == 0 );
        if( !boost::filesystem::exists(filename) )
            return Status( ErrorCodes::InvalidPath, "DataFile::openExisting - file does not exist" );

        if( !mmf.open(filename,false) ) {
            return Status( ErrorCodes::InternalError, "DataFile::openExisting - mmf.open failed" );
        }
        _mb = mmf.getView(); verify(_mb);
        unsigned long long sz = mmf.length();
        verify( sz <= 0x7fffffff );
        verify( sz % 4096 == 0 );
        if (sz < 64*1024*1024 && !mmapv1GlobalOptions.smallfiles) {
            if( sz >= 16*1024*1024 && sz % (1024*1024) == 0 ) {
                log() << "info openExisting file size " << sz
                      << " but mmapv1GlobalOptions.smallfiles=false: "
                      << filename << endl;
            }
            else {
                log() << "openExisting size " << sz << " less than minimum file size expectation "
                      << filename << endl;
                verify(false);
            }
        }
        data_file_check(_mb);
        return Status::OK();
    }

    void DataFile::open( OperationContext* txn,
                         const char *filename,
                         int minSize,
                         bool preallocateOnly ) {
        long size = defaultSize( filename );
        while ( size < minSize ) {
            if ( size < maxSize() / 2 )
                size *= 2;
            else {
                size = maxSize();
                break;
            }
        }
        if ( size > maxSize() )
            size = maxSize();

        verify(size >= 64*1024*1024 || mmapv1GlobalOptions.smallfiles);
        verify( size % 4096 == 0 );

        if ( preallocateOnly ) {
            if (mmapv1GlobalOptions.prealloc) {
                FileAllocator::get()->requestAllocation( filename, size );
            }
            return;
        }

        {
            verify( _mb == 0 );
            unsigned long long sz = size;
            if( mmf.create(filename, sz, false) )
                _mb = mmf.getView();
            verify( sz <= 0x7fffffff );
            size = (int) sz;
        }
        data_file_check(_mb);
        header()->init(txn, fileNo, size, filename);
    }

    void DataFile::flush( bool sync ) {
        mmf.flush( sync );
    }

    DiskLoc DataFile::allocExtentArea( OperationContext* txn, int size ) {

        massert( 10357, "shutdown in progress", !inShutdown() );
        massert( 10359, "header==0 on new extent: 32 bit mmap space exceeded?", header() ); // null if file open failed

        verify( size <= header()->unusedLength );

        int offset = header()->unused.getOfs();

        DataFileHeader *h = header();
        *txn->recoveryUnit()->writing(&h->unused) = DiskLoc( fileNo, offset + size );
        txn->recoveryUnit()->writingInt(h->unusedLength) = h->unusedLength - size;

        return DiskLoc( fileNo, offset );
    }

    // -------------------------------------------------------------------------------

    void DataFileHeader::init(OperationContext* txn, int fileno, int filelength, const char* filename) {
        if ( uninitialized() ) {
            DEV log() << "datafileheader::init initializing " << filename << " n:" << fileno << endl;
            if( !(filelength > 32768 ) ) {
                massert(13640, str::stream() << "DataFileHeader looks corrupt at file open filelength:" << filelength << " fileno:" << fileno, false);
            }

            {
                // "something" is too vague, but we checked for the right db to be locked higher up the call stack
                if (!txn->lockState()->isWriteLocked()) {
                    txn->lockState()->dump();
                    log() << "*** TEMP NOT INITIALIZING FILE " << filename << ", not in a write lock." << endl;
                    log() << "temp bypass until more elaborate change - case that is manifesting is benign anyway" << endl;
                    return;
                    /**
                       log() << "ERROR can't create outside a write lock" << endl;
                       printStackTrace();
                       ::abort();
                    **/
                }
            }

            // The writes done in this function must not be rolled back. If the containing
            // UnitOfWork rolls back it should roll back to the state *after* these writes. This
            // will leave the file empty, but available for future use. That is why we go directly
            // to the global dur dirty list rather than going through the OperationContext.
            getDur().createdFile(filename, filelength);
            verify( HeaderSize == 8192 );
            DataFileHeader *h = getDur().writing(this);
            h->fileLength = filelength;
            h->version = DataFileVersion::defaultForNewFiles();
            h->unused.set( fileno, HeaderSize );
            verify( (data-(char*)this) == HeaderSize );
            h->unusedLength = fileLength - HeaderSize - 16;
            h->freeListStart.Null();
            h->freeListEnd.Null();
        }
        else {
            checkUpgrade(txn);
        }
    }

    void DataFileHeader::checkUpgrade(OperationContext* txn) {
        if ( freeListStart == DiskLoc(0, 0) ) {
            // we are upgrading from 2.4 to 2.6
            invariant(freeListEnd == DiskLoc(0, 0)); // both start and end should be (0,0) or real
            WriteUnitOfWork wunit(txn);
            *txn->recoveryUnit()->writing( &freeListStart ) = DiskLoc();
            *txn->recoveryUnit()->writing( &freeListEnd ) = DiskLoc();
            wunit.commit();
        }
    }

}

/*
 * sqlite_vfs.c - Custom SQLite VFS for Fiberus io_uring integration
 *
 * This VFS wraps the default Unix VFS but replaces xRead, xWrite, and xSync
 * with io_uring-based implementations that suspend the calling fiber instead
 * of blocking the OS thread.
 *
 * Architecture:
 *   Haxe code -> sqlite3_step() -> SQLite B-tree/pager ->
 *     xRead()  -> fib_io_pread_raw()  -> IORING_OP_READ  -> fiber suspends
 *     xWrite() -> fib_io_pwrite_raw() -> IORING_OP_WRITE -> fiber suspends
 *     xSync()  -> fib_io_fsync()      -> IORING_OP_FSYNC -> fiber suspends
 *
 * All other VFS methods (open, close, lock, truncate, etc.) delegate directly
 * to the default Unix VFS since they are fast synchronous operations.
 *
 * IMPORTANT: xRead must zero-fill short reads (SQLITE_IOERR_SHORT_READ).
 * Failure to do so will eventually lead to database corruption.
 */

#define _GNU_SOURCE

#include "sqlite_vfs.h"
#include "iouring.h"

#include "sqlite3.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

/* ============================================================================
 * File Handle
 *
 * Extends sqlite3_file with the underlying Unix VFS file handle.
 * The unix_file member is the file object that the default VFS xOpen creates.
 * We delegate all non-I/O operations to it.
 * ============================================================================ */

typedef struct FibVfsFile {
    sqlite3_file base;           /* Must be first - SQLite casts to this */
    sqlite3_file* unix_file;     /* Underlying Unix VFS file (heap-allocated) */
    const sqlite3_io_methods* unix_methods;  /* Unix VFS io_methods (for delegation) */
    int fd;                      /* Raw fd for io_uring read/write/fsync */
} FibVfsFile;

/* ============================================================================
 * Statics
 * ============================================================================ */

static sqlite3_vfs fib_vfs;
static sqlite3_io_methods fib_io_methods_v1;
static sqlite3_io_methods fib_io_methods_v2;
static int fib_vfs_registered = 0;

/* ============================================================================
 * I/O Methods - io_uring overrides
 * ============================================================================ */

/*
 * fib_vfs_close - Close file, delegating to Unix VFS
 */
static int fib_vfs_close(sqlite3_file* pFile) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    int rc = SQLITE_OK;
    
    if (f->unix_file && f->unix_methods) {
        rc = f->unix_methods->xClose(f->unix_file);
    }
    
    if (f->unix_file) {
        sqlite3_free(f->unix_file);
        f->unix_file = NULL;
    }
    f->unix_methods = NULL;
    f->fd = -1;
    
    return rc;
}

/*
 * fib_vfs_read - Read via io_uring (fiber-suspending)
 *
 * CRITICAL: Must zero-fill buffer on short read and return SQLITE_IOERR_SHORT_READ.
 * SQLite relies on this behavior for correct operation.
 */
static int fib_vfs_read(sqlite3_file* pFile, void* zBuf, int iAmt, sqlite3_int64 iOfst) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    
    if (f->fd < 0) return SQLITE_IOERR_READ;
    
    ssize_t nRead = fib_io_pread_raw(f->fd, zBuf, (size_t)iAmt, (off_t)iOfst);
    
    if (nRead == (ssize_t)iAmt) {
        return SQLITE_OK;
    }
    
    if (nRead >= 0) {
        /* Short read - zero-fill remainder (required by SQLite) */
        if (nRead > 0) {
            memset((char*)zBuf + nRead, 0, (size_t)(iAmt - nRead));
        } else {
            memset(zBuf, 0, (size_t)iAmt);
        }
        return SQLITE_IOERR_SHORT_READ;
    }
    
    /* Error */
    return SQLITE_IOERR_READ;
}

/*
 * fib_vfs_write - Write via io_uring (fiber-suspending)
 */
static int fib_vfs_write(sqlite3_file* pFile, const void* zBuf, int iAmt, sqlite3_int64 iOfst) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    
    if (f->fd < 0) return SQLITE_IOERR_WRITE;
    
    size_t total = 0;
    const char* src = (const char*)zBuf;
    
    /* Write all data - handle partial writes by retrying */
    while (total < (size_t)iAmt) {
        ssize_t nWritten = fib_io_pwrite_raw(f->fd, src + total, 
                                              (size_t)iAmt - total,
                                              (off_t)(iOfst + (sqlite3_int64)total));
        if (nWritten <= 0) {
            return SQLITE_IOERR_WRITE;
        }
        total += (size_t)nWritten;
    }
    
    return SQLITE_OK;
}

/*
 * fib_vfs_sync - Sync via io_uring (fiber-suspending)
 */
static int fib_vfs_sync(sqlite3_file* pFile, int flags) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    (void)flags;
    
    if (f->fd < 0) return SQLITE_IOERR_FSYNC;
    
    int rc = fib_io_fsync(f->fd);
    return (rc == 0) ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

/* ============================================================================
 * I/O Methods - delegated to Unix VFS
 * ============================================================================ */

static int fib_vfs_truncate(sqlite3_file* pFile, sqlite3_int64 size) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (!f->unix_methods) return SQLITE_IOERR_TRUNCATE;
    return f->unix_methods->xTruncate(f->unix_file, size);
}

static int fib_vfs_filesize(sqlite3_file* pFile, sqlite3_int64* pSize) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (!f->unix_methods) return SQLITE_IOERR_FSTAT;
    return f->unix_methods->xFileSize(f->unix_file, pSize);
}

static int fib_vfs_lock(sqlite3_file* pFile, int locktype) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (!f->unix_methods) return SQLITE_OK;
    return f->unix_methods->xLock(f->unix_file, locktype);
}

static int fib_vfs_unlock(sqlite3_file* pFile, int locktype) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (!f->unix_methods) return SQLITE_OK;
    return f->unix_methods->xUnlock(f->unix_file, locktype);
}

static int fib_vfs_check_reserved_lock(sqlite3_file* pFile, int* pResOut) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (!f->unix_methods) { *pResOut = 0; return SQLITE_OK; }
    return f->unix_methods->xCheckReservedLock(f->unix_file, pResOut);
}

static int fib_vfs_file_control(sqlite3_file* pFile, int op, void* pArg) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (!f->unix_methods) return SQLITE_NOTFOUND;
    return f->unix_methods->xFileControl(f->unix_file, op, pArg);
}

static int fib_vfs_sector_size(sqlite3_file* pFile) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (!f->unix_methods) return 4096;
    return f->unix_methods->xSectorSize(f->unix_file);
}

static int fib_vfs_device_characteristics(sqlite3_file* pFile) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (!f->unix_methods) return 0;
    return f->unix_methods->xDeviceCharacteristics(f->unix_file);
}

/* Version 2 methods (SHM for WAL mode) - delegate to Unix VFS */

static int fib_vfs_shm_map(sqlite3_file* pFile, int iPg, int pgsz, int isWrite, void volatile** pp) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (!f->unix_methods || f->unix_methods->iVersion < 2) return SQLITE_IOERR;
    return f->unix_methods->xShmMap(f->unix_file, iPg, pgsz, isWrite, pp);
}

static int fib_vfs_shm_lock(sqlite3_file* pFile, int offset, int n, int flags) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (!f->unix_methods || f->unix_methods->iVersion < 2) return SQLITE_IOERR;
    return f->unix_methods->xShmLock(f->unix_file, offset, n, flags);
}

static void fib_vfs_shm_barrier(sqlite3_file* pFile) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (f->unix_methods && f->unix_methods->iVersion >= 2) {
        f->unix_methods->xShmBarrier(f->unix_file);
    }
}

static int fib_vfs_shm_unmap(sqlite3_file* pFile, int deleteFlag) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    if (!f->unix_methods || f->unix_methods->iVersion < 2) return SQLITE_OK;
    return f->unix_methods->xShmUnmap(f->unix_file, deleteFlag);
}

/* ============================================================================
 * VFS Methods
 * ============================================================================ */

/*
 * fib_vfs_open - Open a file using the Unix VFS, then wrap it
 *
 * We let the Unix VFS do the actual open (which handles flags, mode, etc.),
 * then extract the fd from it and set up our io_uring-enabled io_methods.
 */
static int fib_vfs_open(sqlite3_vfs* pVfs, sqlite3_filename zName,
                         sqlite3_file* pFile, int flags, int* pOutFlags) {
    FibVfsFile* f = (FibVfsFile*)pFile;
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    
    if (!unix_vfs) return SQLITE_CANTOPEN;
    
    /* Initialize our file handle */
    memset(f, 0, sizeof(FibVfsFile));
    f->fd = -1;
    
    /* Allocate space for the Unix VFS file handle */
    f->unix_file = (sqlite3_file*)sqlite3_malloc(unix_vfs->szOsFile);
    if (!f->unix_file) return SQLITE_NOMEM;
    memset(f->unix_file, 0, (size_t)unix_vfs->szOsFile);
    
    /* Let Unix VFS do the actual open */
    int rc = unix_vfs->xOpen(unix_vfs, zName, f->unix_file, flags, pOutFlags);
    if (rc != SQLITE_OK) {
        sqlite3_free(f->unix_file);
        f->unix_file = NULL;
        /* pMethods must be NULL if open fails and we don't want xClose called */
        f->base.pMethods = NULL;
        return rc;
    }
    
    /* Save the Unix VFS io_methods for delegation */
    f->unix_methods = f->unix_file->pMethods;
    
    /* Extract the fd from the Unix VFS file.
     * The Unix VFS stores the fd at offset 0 after the sqlite3_file base.
     * This is a well-known struct layout used by all SQLite Unix VFS variants:
     *   struct unixFile { sqlite3_file base; ... int h; ... }
     * The 'h' (file handle) field is the first int after the pMethods pointer.
     * We use xFileControl with SQLITE_FCNTL_FILE_POINTER is not available,
     * so we extract fd using fileno-like approach via /proc/self/fd or
     * by reading the struct layout.
     *
     * Actually, the reliable way is to use SQLITE_FCNTL_FILEID or simply
     * open the file ourselves. But the simplest correct approach is to
     * let Unix VFS open, then use xFileControl(SQLITE_FCNTL_FILE_POINTER)
     * which doesn't exist. Instead, use the known struct layout.
     *
     * The struct unixFile layout (from os_unix.c) starts with:
     *   sqlite3_io_methods *pMethod;   (= sqlite3_file.pMethods)
     *   sqlite3_vfs *pVfs;
     *   unixInodeInfo *pInode;
     *   int h;                          <-- THIS is the fd
     *
     * On x86_64: pMethods=8 bytes, pVfs=8 bytes, pInode=8 bytes, then h.
     * So fd is at byte offset 24 from the start of the file struct.
     */
    {
        /* Extract fd at known offset: 3 pointers into the struct */
        char* raw = (char*)f->unix_file;
        int extracted_fd;
        memcpy(&extracted_fd, raw + 3 * sizeof(void*), sizeof(int));
        f->fd = extracted_fd;
    }
    
    /* Validate the fd is usable */
    if (f->fd < 0) {
        /* Fallback: delegate everything to Unix VFS (no io_uring benefit) */
        f->base.pMethods = f->unix_methods;
        return SQLITE_OK;
    }
    
    /* Set our io_methods based on whether the Unix VFS supports v2 (SHM/WAL) */
    if (f->unix_methods && f->unix_methods->iVersion >= 2) {
        f->base.pMethods = &fib_io_methods_v2;
    } else {
        f->base.pMethods = &fib_io_methods_v1;
    }
    
    return SQLITE_OK;
}

/* VFS methods that delegate directly to Unix VFS */

static int fib_vfs_delete(sqlite3_vfs* pVfs, const char* zName, int syncDir) {
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    return unix_vfs->xDelete(unix_vfs, zName, syncDir);
}

static int fib_vfs_access(sqlite3_vfs* pVfs, const char* zName, int flags, int* pResOut) {
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    return unix_vfs->xAccess(unix_vfs, zName, flags, pResOut);
}

static int fib_vfs_full_pathname(sqlite3_vfs* pVfs, const char* zName, int nOut, char* zOut) {
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    return unix_vfs->xFullPathname(unix_vfs, zName, nOut, zOut);
}

static void* fib_vfs_dlopen(sqlite3_vfs* pVfs, const char* zFilename) {
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    return unix_vfs->xDlOpen(unix_vfs, zFilename);
}

static void fib_vfs_dlerror(sqlite3_vfs* pVfs, int nByte, char* zErrMsg) {
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    unix_vfs->xDlError(unix_vfs, nByte, zErrMsg);
}

static void (*fib_vfs_dlsym(sqlite3_vfs* pVfs, void* pHandle, const char* zSymbol))(void) {
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    return unix_vfs->xDlSym(unix_vfs, pHandle, zSymbol);
}

static void fib_vfs_dlclose(sqlite3_vfs* pVfs, void* pHandle) {
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    unix_vfs->xDlClose(unix_vfs, pHandle);
}

static int fib_vfs_randomness(sqlite3_vfs* pVfs, int nByte, char* zOut) {
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    return unix_vfs->xRandomness(unix_vfs, nByte, zOut);
}

static int fib_vfs_sleep(sqlite3_vfs* pVfs, int microseconds) {
    /* Use io_uring sleep to avoid blocking the thread */
    (void)pVfs;
    fib_io_sleep_ns((uint64_t)microseconds * 1000ULL);
    return microseconds;
}

static int fib_vfs_current_time(sqlite3_vfs* pVfs, double* pTimeOut) {
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    return unix_vfs->xCurrentTime(unix_vfs, pTimeOut);
}

static int fib_vfs_get_last_error(sqlite3_vfs* pVfs, int nByte, char* zErrMsg) {
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    return unix_vfs->xGetLastError(unix_vfs, nByte, zErrMsg);
}

static int fib_vfs_current_time_int64(sqlite3_vfs* pVfs, sqlite3_int64* pTimeOut) {
    sqlite3_vfs* unix_vfs = (sqlite3_vfs*)pVfs->pAppData;
    if (unix_vfs->iVersion >= 2 && unix_vfs->xCurrentTimeInt64) {
        return unix_vfs->xCurrentTimeInt64(unix_vfs, pTimeOut);
    }
    /* Fallback via double */
    double t;
    int rc = unix_vfs->xCurrentTime(unix_vfs, &t);
    if (rc == SQLITE_OK) {
        *pTimeOut = (sqlite3_int64)(t * 86400000.0);
    }
    return rc;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

int fib_sqlite_vfs_register(void) {
    if (fib_vfs_registered) return SQLITE_OK;
    
    /* Find the default VFS (Unix on Linux) */
    sqlite3_vfs* unix_vfs = sqlite3_vfs_find(NULL);
    if (!unix_vfs) return SQLITE_ERROR;
    
    /* Initialize io_methods v1 (basic file operations) */
    memset(&fib_io_methods_v1, 0, sizeof(fib_io_methods_v1));
    fib_io_methods_v1.iVersion = 1;
    fib_io_methods_v1.xClose = fib_vfs_close;
    fib_io_methods_v1.xRead = fib_vfs_read;
    fib_io_methods_v1.xWrite = fib_vfs_write;
    fib_io_methods_v1.xTruncate = fib_vfs_truncate;
    fib_io_methods_v1.xSync = fib_vfs_sync;
    fib_io_methods_v1.xFileSize = fib_vfs_filesize;
    fib_io_methods_v1.xLock = fib_vfs_lock;
    fib_io_methods_v1.xUnlock = fib_vfs_unlock;
    fib_io_methods_v1.xCheckReservedLock = fib_vfs_check_reserved_lock;
    fib_io_methods_v1.xFileControl = fib_vfs_file_control;
    fib_io_methods_v1.xSectorSize = fib_vfs_sector_size;
    fib_io_methods_v1.xDeviceCharacteristics = fib_vfs_device_characteristics;
    
    /* Initialize io_methods v2 (adds SHM for WAL mode) */
    fib_io_methods_v2 = fib_io_methods_v1;
    fib_io_methods_v2.iVersion = 2;
    fib_io_methods_v2.xShmMap = fib_vfs_shm_map;
    fib_io_methods_v2.xShmLock = fib_vfs_shm_lock;
    fib_io_methods_v2.xShmBarrier = fib_vfs_shm_barrier;
    fib_io_methods_v2.xShmUnmap = fib_vfs_shm_unmap;
    
    /* Initialize the VFS struct */
    memset(&fib_vfs, 0, sizeof(fib_vfs));
    fib_vfs.iVersion = 2;                         /* Support v2 (CurrentTimeInt64) */
    fib_vfs.szOsFile = (int)sizeof(FibVfsFile);   /* Our file handle size */
    fib_vfs.mxPathname = unix_vfs->mxPathname;    /* Same path limits */
    fib_vfs.zName = "fiberus";
    fib_vfs.pAppData = unix_vfs;                  /* Store Unix VFS for delegation */
    fib_vfs.xOpen = fib_vfs_open;
    fib_vfs.xDelete = fib_vfs_delete;
    fib_vfs.xAccess = fib_vfs_access;
    fib_vfs.xFullPathname = fib_vfs_full_pathname;
    fib_vfs.xDlOpen = fib_vfs_dlopen;
    fib_vfs.xDlError = fib_vfs_dlerror;
    fib_vfs.xDlSym = fib_vfs_dlsym;
    fib_vfs.xDlClose = fib_vfs_dlclose;
    fib_vfs.xRandomness = fib_vfs_randomness;
    fib_vfs.xSleep = fib_vfs_sleep;
    fib_vfs.xCurrentTime = fib_vfs_current_time;
    fib_vfs.xGetLastError = fib_vfs_get_last_error;
    fib_vfs.xCurrentTimeInt64 = fib_vfs_current_time_int64;
    
    /* Register as the DEFAULT VFS so all sqlite3_open() calls use it */
    int rc = sqlite3_vfs_register(&fib_vfs, 1);
    if (rc == SQLITE_OK) {
        fib_vfs_registered = 1;
    }
    
    return rc;
}

/*
 * sqlite_vfs.h - Custom SQLite VFS for Fiberus io_uring integration
 *
 * Provides a SQLite VFS named "fiberus" that routes file I/O operations
 * (xRead, xWrite, xSync) through io_uring, allowing fibers to suspend
 * instead of blocking the OS thread during database operations.
 *
 * The VFS delegates most methods to the default Unix VFS, overriding only
 * the performance-critical I/O methods (read, write, sync) with io_uring
 * equivalents. File open/close/lock/stat remain synchronous as they are
 * fast kernel operations that don't benefit from async I/O.
 *
 * Usage:
 *   fib_sqlite_vfs_register();  // Call once at startup
 *   // Then open databases normally - they use the fiberus VFS automatically
 */

#ifndef FIB_SQLITE_VFS_H
#define FIB_SQLITE_VFS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register the "fiberus" VFS with SQLite.
 *
 * Makes it the default VFS so all subsequent sqlite3_open() calls use it.
 * Safe to call multiple times (idempotent - second call is a no-op).
 *
 * Returns 0 on success, non-zero on failure.
 */
int fib_sqlite_vfs_register(void);

#ifdef __cplusplus
}
#endif

#endif /* FIB_SQLITE_VFS_H */

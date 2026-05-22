/*
 * fib_sqlite_mutex.h - Spin-only fiber-aware SQLite mutex backend
 *
 * Provides a sqlite3_mutex_methods vtable that uses spin + cooperative
 * fiber_yield() instead of OS-thread blocking. Required when SQLite is
 * compiled with SQLITE_THREADSAFE=1 inside fiberus — without this, the
 * default pthread mutex backend would OS-thread-block the carrier on
 * contention, defeating the work-stealing scheduler.
 *
 * Design choice — spin-only with no parking:
 *   - No FIBER_STATE_WAITING transition (avoids the fiberus push-before-
 *     switch race that killed the previous Counter-based attempt).
 *   - fiber_yield() is the well-tested cooperative-yield path that
 *     re-pushes the fiber to its OWN queue; no cross-thread wake.
 *   - SQLite holds its mutex for µs per call (ms during VFS I/O).
 *     Spin-yield throttles contention with sub-1% CPU overhead.
 *
 * Install once at startup BEFORE any sqlite3_initialize() call:
 *   sqlite3_config(SQLITE_CONFIG_MUTEX, &fib_sqlite_mutex_methods);
 */

#ifndef FIB_SQLITE_MUTEX_H
#define FIB_SQLITE_MUTEX_H

#include "sqlite3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vtable passed to sqlite3_config(SQLITE_CONFIG_MUTEX, ...). */
extern const sqlite3_mutex_methods fib_sqlite_mutex_methods;

#ifdef __cplusplus
}
#endif

#endif /* FIB_SQLITE_MUTEX_H */

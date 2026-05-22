/*
 * fib_sqlite_mutex.c - Spin-only fiber-aware SQLite mutex backend
 *
 * See header for design rationale. Key differences from the failed
 * Counter-based attempt (V1):
 *   - No waitlist, no parking, no Counter, no scheduler_register_suspended.
 *   - Nothing ever transitions to FIBER_STATE_WAITING → the push-before-
 *     switch race surface that killed V1 is fully eliminated.
 *   - Contending fibers spin with cooperative fiber_yield() every 8 CAS
 *     failures. The scheduler re-pushes them to their own work queue;
 *     when the holder releases, the next CAS attempt succeeds.
 *
 * Recursive ownership: tracked by Fiber* pointer (stable across carrier
 * migration). SQLite's recursive contract is "same logical caller" —
 * our fiber identity satisfies this even when the fiber migrates
 * between OS threads mid-call (e.g. yielding via io_uring xRead).
 */

#define _GNU_SOURCE
#include "fib_sqlite_mutex.h"
#include "sqlite3.h"

#include "../../../fiberus/runtime/fiber.h"
#include "../../../fiberus/runtime/scheduler.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Mutex wrapper struct (plain malloc'd; no GC interaction)
 * ========================================================================== */

typedef struct FibSqliteMutex {
    volatile int state;             /* 0 = free, 1 = held */
    volatile uintptr_t owner;       /* current owner Fiber* (0 if free) */
    volatile int lock_count;        /* recursive entry depth */
    int is_recursive;               /* SQLITE_MUTEX_RECURSIVE vs FAST */
    int static_id;                  /* 0 for dynamic; >0 for static singletons */
} FibSqliteMutex;

/* ============================================================================
 * Static mutex table — SQLite hands integer IDs (SQLITE_MUTEX_STATIC_MASTER
 * = 2 through ~11) and expects xMutexAlloc(id) to return the SAME pointer
 * each time. Pre-allocate lazily under a one-shot 3-state atomic guard
 * (same pattern fib_sqlite_ensure_vfs_registered uses).
 * ========================================================================== */

#define FIB_STATIC_MUTEX_MAX 32  /* generous; SQLite uses ~13 */
static FibSqliteMutex* static_mutex_table[FIB_STATIC_MUTEX_MAX] = {0};
static volatile int static_table_init = 0;

static FibSqliteMutex* alloc_mutex_struct(int is_recursive, int static_id) {
    FibSqliteMutex* fm = (FibSqliteMutex*)calloc(1, sizeof(FibSqliteMutex));
    if (!fm) return NULL;
    fm->is_recursive = is_recursive;
    fm->static_id = static_id;
    return fm;
}

/* ============================================================================
 * SQLite mutex_methods implementations
 * ========================================================================== */

static int fib_mutex_init_impl(void) {
    /* Lazy table init in xAlloc — no global state to initialize here. */
    return SQLITE_OK;
}

static int fib_mutex_end_impl(void) {
    /* Never free static mutexes — sqlite3_shutdown can be followed by
     * sqlite3_initialize in long-running processes. Process exit reclaims. */
    return SQLITE_OK;
}

static sqlite3_mutex* fib_mutex_alloc_impl(int type) {
    /* Dynamic mutexes — fresh allocation each call. */
    if (type == SQLITE_MUTEX_FAST) {
        return (sqlite3_mutex*)alloc_mutex_struct(0, 0);
    }
    if (type == SQLITE_MUTEX_RECURSIVE) {
        return (sqlite3_mutex*)alloc_mutex_struct(1, 0);
    }

    /* Static mutexes — return the same pointer for the same id. */
    if (type < 0 || type >= FIB_STATIC_MUTEX_MAX) {
        return NULL;
    }

    int expected = 0;
    if (__atomic_compare_exchange_n(&static_table_init, &expected, 1,
                                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        /* We are the initializer — populate the entire table. */
        for (int i = 0; i < FIB_STATIC_MUTEX_MAX; i++) {
            /* SQLite expects static mutexes to be recursive. */
            static_mutex_table[i] = alloc_mutex_struct(1, i);
        }
        __atomic_store_n(&static_table_init, 2, __ATOMIC_RELEASE);
    } else {
        /* Lost the init race — spin briefly until ready. */
        while (__atomic_load_n(&static_table_init, __ATOMIC_ACQUIRE) != 2) {
            /* Microsecond busy wait — table init is microseconds. */
        }
    }

    return (sqlite3_mutex*)static_mutex_table[type];
}

static void fib_mutex_free_impl(sqlite3_mutex* m) {
    FibSqliteMutex* fm = (FibSqliteMutex*)m;
    if (!fm) return;
    /* Static mutexes live for the process lifetime — SQLite won't call
     * us back for those, but defend against it anyway. */
    if (fm->static_id != 0) return;
    free(fm);
}

static void fib_mutex_enter_impl(sqlite3_mutex* m) {
    if (!m) return;
    FibSqliteMutex* fm = (FibSqliteMutex*)m;
    Fiber* self = fiber_current();

    /* Recursive re-entry by the same fiber — no CAS needed. */
    if (fm->is_recursive && self &&
        __atomic_load_n(&fm->owner, __ATOMIC_ACQUIRE) == (uintptr_t)self) {
        __atomic_fetch_add(&fm->lock_count, 1, __ATOMIC_ACQ_REL);
        return;
    }

    /* Spin-with-yield. Yield every 8 failed CAS attempts so the carrier
     * doesn't burn CPU on contended waits AND the holder isn't lapped
     * by contenders constantly re-pushing themselves to the work queue.
     * Empirically yield-every-1 was worse (livelock + lost-serialization
     * under wsdeque pressure); yield-every-8 is the sweet spot matching
     * the in-production WriteLock.spinLock pattern. */
    int spins = 0;
    int expected;
    while (1) {
        expected = 0;
        if (__atomic_compare_exchange_n(&fm->state, &expected, 1,
                                        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            __atomic_store_n(&fm->owner, (uintptr_t)self, __ATOMIC_RELEASE);
            __atomic_store_n(&fm->lock_count, 1, __ATOMIC_RELEASE);
            return;
        }
        spins++;
        if ((spins & 0x7) == 0 && self && tls_scheduler) {
            fiber_yield();
        }
    }
}

static int fib_mutex_try_impl(sqlite3_mutex* m) {
    if (!m) return SQLITE_OK;
    FibSqliteMutex* fm = (FibSqliteMutex*)m;
    Fiber* self = fiber_current();

    if (fm->is_recursive && self &&
        __atomic_load_n(&fm->owner, __ATOMIC_ACQUIRE) == (uintptr_t)self) {
        __atomic_fetch_add(&fm->lock_count, 1, __ATOMIC_ACQ_REL);
        return SQLITE_OK;
    }

    int expected = 0;
    if (__atomic_compare_exchange_n(&fm->state, &expected, 1,
                                    0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_store_n(&fm->owner, (uintptr_t)self, __ATOMIC_RELEASE);
        __atomic_store_n(&fm->lock_count, 1, __ATOMIC_RELEASE);
        return SQLITE_OK;
    }
    return SQLITE_BUSY;
}

static void fib_mutex_leave_impl(sqlite3_mutex* m) {
    if (!m) return;
    FibSqliteMutex* fm = (FibSqliteMutex*)m;

    if (fm->is_recursive) {
        int prev = __atomic_fetch_sub(&fm->lock_count, 1, __ATOMIC_ACQ_REL);
        if (prev > 1) return;
    } else {
        __atomic_store_n(&fm->lock_count, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&fm->owner, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&fm->state, 0, __ATOMIC_RELEASE);
}

static int fib_mutex_held_impl(sqlite3_mutex* m) {
    /* SQLite uses this only in debug assertions. NULL is treated as
     * "held" per SQLite convention. */
    if (!m) return 1;
    FibSqliteMutex* fm = (FibSqliteMutex*)m;
    Fiber* self = fiber_current();
    if (!self) return 0;
    return (__atomic_load_n(&fm->owner, __ATOMIC_ACQUIRE) == (uintptr_t)self) ? 1 : 0;
}

static int fib_mutex_notheld_impl(sqlite3_mutex* m) {
    if (!m) return 1;
    return !fib_mutex_held_impl(m);
}

/* ============================================================================
 * Exported vtable
 * ========================================================================== */

const sqlite3_mutex_methods fib_sqlite_mutex_methods = {
    fib_mutex_init_impl,
    fib_mutex_end_impl,
    fib_mutex_alloc_impl,
    fib_mutex_free_impl,
    fib_mutex_enter_impl,
    fib_mutex_try_impl,
    fib_mutex_leave_impl,
    fib_mutex_held_impl,
    fib_mutex_notheld_impl
};

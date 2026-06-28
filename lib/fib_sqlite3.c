/*
 * fib_sqlite3.c - SQLite3 bindings for Fiberus
 *
 * Part of the fib_sqlite3 library (libraries/fib_sqlite3).
 * Implements the C wrapper functions declared in fib_sqlite3.h.
 * All memory returned to Haxe land (FibString, FibBytesData) is
 * GC-managed.
 *
 * FibSqliteDb and FibSqliteStmt wrapper structs are allocated with
 * malloc (not the GC heap) since they contain only raw C pointers
 * that the GC cannot trace. Their internal SQLite resources
 * (sqlite3_stmt, sqlite3 handles) AND the wrapper memory are both
 * released explicitly via close/finalize.
 */

#include "fib_sqlite3.h"
#include "sqlite_vfs.h"
#include "fib_sqlite_mutex.h"
#include "fibstring.h"
#include "bytes.h"

#include "sqlite3.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* gc_pin_object pins cross-fiber-shared objects at depth 0. */
extern void gc_pin_object(void* obj);

/* FibSqliteDb/FibSqliteStmt are malloc'd (not GC-managed) since they
 * contain only raw C pointers that the GC cannot trace.  Using the GC
 * heap (fibrix_alloc) with @:coreType Haxe types caused the GC
 * recycler to treat live wrappers as holes and overwrite them. */

/* One-shot init covers BOTH the fiber-aware mutex backend install AND
 * the io_uring VFS registration. Use an atomic 3-state flag:
 * 0=uninitialized, 1=initializing, 2=ready.
 *
 * Ordering inside the init block matters:
 *   1. sqlite3_config(SQLITE_CONFIG_MUTEX, &fib_sqlite_mutex_methods) —
 *      MUST run BEFORE sqlite3_initialize. SQLite forbids changing the
 *      mutex backend once initialize has run.
 *   2. sqlite3_initialize() — explicit so subsequent sqlite3_* calls
 *      (including the implicit init inside sqlite3_vfs_register) are
 *      no-ops rather than first-init.
 *   3. fib_sqlite_vfs_register() — installs the io_uring VFS.
 *
 * Without (1) before (2), SQLite defaults to its pthread mutex backend,
 * which OS-thread-blocks the carrier on contention. */
static volatile int fib_sqlite_vfs_initialized = 0;

static inline void fib_sqlite_ensure_vfs_registered(void) {
    if (__atomic_load_n(&fib_sqlite_vfs_initialized, __ATOMIC_ACQUIRE) == 2) {
        return;
    }
    int expected = 0;
    if (__atomic_compare_exchange_n(&fib_sqlite_vfs_initialized,
                                    &expected, 1,
                                    0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        sqlite3_config(SQLITE_CONFIG_MUTEX, &fib_sqlite_mutex_methods);
        sqlite3_initialize();
        fib_sqlite_vfs_register();
        __atomic_store_n(&fib_sqlite_vfs_initialized, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&fib_sqlite_vfs_initialized, __ATOMIC_ACQUIRE) != 2) {
            /* Brief spin — init completes in microseconds. */
        }
    }
}

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

struct FibSqliteDb {
    sqlite3* db;
};

struct FibSqliteStmt {
    sqlite3_stmt* stmt;
    FibSqliteDb*  db;            /* Back-pointer to owning connection (for errmsg) */
    /* Column name cache: caches the raw const char* pointers from
     * sqlite3_column_name (which returns pointers to SQLite's internal
     * persistent storage, valid for the prepared statement's lifetime).
     *
     * Each fib_sqlite_column_name call creates a fresh fiber-local
     * FibString from the cached C string — NO gc_pin_object, NO membrane
     * capability, NO static root registration. The FibString lives only
     * for the caller's request scope and is collected normally on fiber
     * exit, unblocking the region-free fast path.
     *
     * This replaces the original code which called fib_string_new +
     * gc_pin_object on EVERY column-name read (one per row × column per
     * handler), producing ~1.8M capabilities per tecdoc-api load run. */
    const char**  column_names;
    int           column_count;
};

/* ============================================================================
 * Database Connection
 * ============================================================================ */

FibSqliteDb* fib_sqlite_open(const char* path) {
    if (!path) return NULL;

    /* Lazily + atomically register the Fiberus io_uring VFS on first open */
    fib_sqlite_ensure_vfs_registered();

    sqlite3* db = NULL;
    /* Open using the "fiberus" VFS - routes read/write/sync through io_uring.
     * If VFS registration failed, sqlite3_open_v2 falls back to the default VFS. */
    int rc = sqlite3_open_v2(path, &db, 
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "fiberus");
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }

    FibSqliteDb* fdb = (FibSqliteDb*)malloc(sizeof(FibSqliteDb));
    if (!fdb) {
        sqlite3_close(db);
        return NULL;
    }
    fdb->db = db;
    return fdb;
}

void fib_sqlite_close(FibSqliteDb* db) {
    if (!db) return;
    if (db->db) {
        sqlite3_close(db->db);
        db->db = NULL;
    }
    free(db);
}

int fib_sqlite_exec(FibSqliteDb* db, const char* sql) {
    if (!db || !db->db || !sql) return -1;
    char* errmsg = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &errmsg);
    if (errmsg) sqlite3_free(errmsg);
    return rc == SQLITE_OK ? 0 : rc;
}

FibString* fib_sqlite_errmsg(FibSqliteDb* db) {
    if (!db || !db->db) return NULL;
    const char* msg = sqlite3_errmsg(db->db);
    if (!msg) return NULL;
    return fib_string_new(msg);
}

int64_t fib_sqlite_last_insert_rowid(FibSqliteDb* db) {
    if (!db || !db->db) return 0;
    return (int64_t)sqlite3_last_insert_rowid(db->db);
}

int fib_sqlite_changes(FibSqliteDb* db) {
    if (!db || !db->db) return 0;
    return sqlite3_changes(db->db);
}

/* Extended result code of the most recent failed API call on this connection.
 * Needed to distinguish SQLITE_BUSY_SNAPSHOT (517) from plain SQLITE_BUSY (5)
 * for BEGIN CONCURRENT conflict-retry. */
int fib_sqlite_extended_errcode(FibSqliteDb* db) {
    if (!db || !db->db) return 0;
    return sqlite3_extended_errcode(db->db);
}

/* ============================================================================
 * Prepared Statements
 * ============================================================================ */

FibSqliteStmt* fib_sqlite_prepare(FibSqliteDb* db, const char* sql) {
    if (!db || !db->db || !sql) return NULL;

    int slen = (int)strlen(sql);
    sqlite3_stmt* stmt = NULL;

    int rc = sqlite3_prepare_v2(db->db, sql, slen, &stmt, NULL);
    if (rc != SQLITE_OK || !stmt) {
        return NULL;
    }

    FibSqliteStmt* fs = (FibSqliteStmt*)malloc(sizeof(FibSqliteStmt));
    if (!fs) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    fs->stmt = stmt;
    fs->db = db;
    fs->column_names = NULL;  /* Populated lazily in fib_sqlite_column_name */
    fs->column_count = 0;
    return fs;
}

int fib_sqlite_step(FibSqliteStmt* stmt) {
    if (!stmt || !stmt->stmt) return -1;
    int rc = sqlite3_step(stmt->stmt);
    if (rc == SQLITE_ROW) return 1;
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

int fib_sqlite_reset(FibSqliteStmt* stmt) {
    if (!stmt) {
        fprintf(stderr, "[SQLITE-RESET] stmt=NULL\n");
        fflush(stderr);
        return -1;
    }
    if (!stmt->stmt) {
        fprintf(stderr, "[SQLITE-RESET] stmt->stmt=NULL (stmt=%p db=%p)\n",
                (void*)stmt, (void*)stmt->db);
        fflush(stderr);
        return -1;
    }
    int rc = sqlite3_reset(stmt->stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[SQLITE-RESET] sqlite3_reset failed rc=%d: %s\n",
                rc, (stmt->db && stmt->db->db) ? sqlite3_errmsg(stmt->db->db) : "no-db");
        fflush(stderr);
        return -1;
    }
    return 0;
}

int fib_sqlite_clear_bindings(FibSqliteStmt* stmt) {
    if (!stmt || !stmt->stmt) return -1;
    return sqlite3_clear_bindings(stmt->stmt) == SQLITE_OK ? 0 : -1;
}

void fib_sqlite_finalize(FibSqliteStmt* stmt) {
    if (!stmt) return;
    if (stmt->stmt) {
        sqlite3_finalize(stmt->stmt);
        stmt->stmt = NULL;
    }
    /* Free the C-string cache array. The const char* pointers inside
     * belong to SQLite (freed when the statement is finalized above). */
    if (stmt->column_names) {
        free(stmt->column_names);
        stmt->column_names = NULL;
        stmt->column_count = 0;
    }
    stmt->db = NULL;
    free(stmt);
}

/* ============================================================================
 * Binding Parameters (1-indexed)
 * ============================================================================ */

int fib_sqlite_bind_int(FibSqliteStmt* stmt, int idx, int value) {
    if (!stmt || !stmt->stmt) return -1;
    return sqlite3_bind_int(stmt->stmt, idx, value) == SQLITE_OK ? 0 : -1;
}

int fib_sqlite_bind_int64(FibSqliteStmt* stmt, int idx, int64_t value) {
    if (!stmt || !stmt->stmt) return -1;
    return sqlite3_bind_int64(stmt->stmt, idx, (sqlite3_int64)value) == SQLITE_OK ? 0 : -1;
}

int fib_sqlite_bind_double(FibSqliteStmt* stmt, int idx, double value) {
    if (!stmt || !stmt->stmt) return -1;
    return sqlite3_bind_double(stmt->stmt, idx, value) == SQLITE_OK ? 0 : -1;
}

int fib_sqlite_bind_text(FibSqliteStmt* stmt, int idx, const char* value) {
    if (!stmt || !stmt->stmt) return -1;
    if (!value) return sqlite3_bind_null(stmt->stmt, idx) == SQLITE_OK ? 0 : -1;
    int len = (int)strlen(value);
    return sqlite3_bind_text(stmt->stmt, idx, value, len, SQLITE_TRANSIENT) == SQLITE_OK ? 0 : -1;
}

int fib_sqlite_bind_blob(FibSqliteStmt* stmt, int idx, FibBytesData* data, int len) {
    if (!stmt || !stmt->stmt) return -1;
    if (!data || len <= 0) return sqlite3_bind_null(stmt->stmt, idx) == SQLITE_OK ? 0 : -1;
    const void* ptr = fib_bytes_data(data);
    return sqlite3_bind_blob(stmt->stmt, idx, ptr, len, SQLITE_TRANSIENT) == SQLITE_OK ? 0 : -1;
}

int fib_sqlite_bind_null(FibSqliteStmt* stmt, int idx) {
    if (!stmt || !stmt->stmt) return -1;
    return sqlite3_bind_null(stmt->stmt, idx) == SQLITE_OK ? 0 : -1;
}

int fib_sqlite_bind_parameter_count(FibSqliteStmt* stmt) {
    if (!stmt || !stmt->stmt) return 0;
    return sqlite3_bind_parameter_count(stmt->stmt);
}

FibString* fib_sqlite_bind_parameter_name(FibSqliteStmt* stmt, int idx) {
    if (!stmt || !stmt->stmt) return NULL;
    const char* name = sqlite3_bind_parameter_name(stmt->stmt, idx);
    if (!name) return NULL;
    return fib_string_new(name);
}

int fib_sqlite_bind_parameter_index(FibSqliteStmt* stmt, const char* name) {
    if (!stmt || !stmt->stmt || !name) return 0;
    return sqlite3_bind_parameter_index(stmt->stmt, name);
}

/* ============================================================================
 * Column Accessors (0-indexed)
 * ============================================================================ */

int fib_sqlite_column_count(FibSqliteStmt* stmt) {
    if (!stmt || !stmt->stmt) return 0;
    return sqlite3_column_count(stmt->stmt);
}

FibString* fib_sqlite_column_name(FibSqliteStmt* stmt, int col) {
    if (!stmt || !stmt->stmt) return NULL;
    const char* name = sqlite3_column_name(stmt->stmt, col);
    if (!name) return NULL;
    return fib_string_new(name);
}

int fib_sqlite_column_type(FibSqliteStmt* stmt, int col) {
    if (!stmt || !stmt->stmt) return 5; /* SQLITE_NULL */
    return sqlite3_column_type(stmt->stmt, col);
}

int fib_sqlite_column_int(FibSqliteStmt* stmt, int col) {
    if (!stmt || !stmt->stmt) return 0;
    return sqlite3_column_int(stmt->stmt, col);
}

int64_t fib_sqlite_column_int64(FibSqliteStmt* stmt, int col) {
    if (!stmt || !stmt->stmt) return 0;
    return (int64_t)sqlite3_column_int64(stmt->stmt, col);
}

double fib_sqlite_column_double(FibSqliteStmt* stmt, int col) {
    if (!stmt || !stmt->stmt) return 0.0;
    return sqlite3_column_double(stmt->stmt, col);
}

FibString* fib_sqlite_column_text(FibSqliteStmt* stmt, int col) {
    if (!stmt || !stmt->stmt) return NULL;
    const unsigned char* text = sqlite3_column_text(stmt->stmt, col);
    if (!text) return NULL;
    int len = sqlite3_column_bytes(stmt->stmt, col);
    return fib_string_new_len((const char*)text, (size_t)len);
}

int fib_sqlite_column_bytes(FibSqliteStmt* stmt, int col) {
    if (!stmt || !stmt->stmt) return 0;
    return sqlite3_column_bytes(stmt->stmt, col);
}

FibBytesData* fib_sqlite_column_blob(FibSqliteStmt* stmt, int col) {
    if (!stmt || !stmt->stmt) return NULL;
    const void* blob = sqlite3_column_blob(stmt->stmt, col);
    int len = sqlite3_column_bytes(stmt->stmt, col);
    if (!blob || len <= 0) return NULL;

    FibBytesData* bd = fib_bytes_alloc(len);
    if (!bd) return NULL;
    memcpy(fib_bytes_data(bd), blob, (size_t)len);
    return bd;
}

int fib_sqlite_column_subtype(FibSqliteStmt* stmt, int col) {
    if (!stmt || !stmt->stmt) return 0;
    sqlite3_value* val = sqlite3_column_value(stmt->stmt, col);
    if (!val) return 0;
    return (int)sqlite3_value_subtype(val);
}

/* ============================================================================
 * GC-Safe Wrappers
 *
 * The fiberus codegen calls fib_string_data() in argument position to extract
 * a const char* from FibString. Between that call and the function body, GC
 * can relocate the FibString, leaving the const char* dangling. The strdup()
 * inside fib_sqlite_open/exec/prepare is too late -- the pointer is already
 * invalid.
 *
 * These wrappers accept FibString* directly. fib_string_data() + strdup()
 * happen in the same statement with no yield point between them, so the
 * const char* cannot be invalidated. The Haxe @:native bindings should point
 * to these wrappers instead of the original const char* functions.
 * ============================================================================ */

FibSqliteDb* fib_sqlite_open_safe(FibString* path_str) {
    if (!path_str) return NULL;
    char* path = strdup(fib_string_data(path_str));
    if (!path) return NULL;

    /* Lazily + atomically register the Fiberus io_uring VFS on first open */
    fib_sqlite_ensure_vfs_registered();

    sqlite3* db = NULL;
    int rc = sqlite3_open_v2(path, &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "fiberus");
    free(path);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }

    FibSqliteDb* fdb = (FibSqliteDb*)malloc(sizeof(FibSqliteDb));
    if (!fdb) {
        sqlite3_close(db);
        return NULL;
    }
    fdb->db = db;
    return fdb;
}

int fib_sqlite_exec_safe(FibSqliteDb* db, FibString* sql_str) {
    if (!db || !db->db || !sql_str) return -1;
    char* sql = strdup(fib_string_data(sql_str));
    if (!sql) return -1;
    char* errmsg = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &errmsg);
    free(sql);
    if (errmsg) sqlite3_free(errmsg);
    return rc == SQLITE_OK ? 0 : rc;
}

FibSqliteStmt* fib_sqlite_prepare_safe(FibSqliteDb* db, FibString* sql_str) {
    if (!db || !db->db || !sql_str) return NULL;
    char* sql = strdup(fib_string_data(sql_str));
    if (!sql) return NULL;

    int slen = (int)strlen(sql);
    sqlite3_stmt* stmt = NULL;

    int rc = sqlite3_prepare_v2(db->db, sql, slen, &stmt, NULL);
    free(sql);
    if (rc != SQLITE_OK || !stmt) {
        return NULL;
    }

    FibSqliteStmt* fs = (FibSqliteStmt*)malloc(sizeof(FibSqliteStmt));
    if (!fs) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    fs->stmt = stmt;
    fs->db = db;
    fs->column_names = NULL;
    fs->column_count = 0;
    return fs;
}

/* ============================================================================
 * Binding Parameters (1-indexed)
 * ============================================================================ */

int fib_sqlite_db_is_null(FibSqliteDb* db) {
    return db == NULL ? 1 : 0;
}

int fib_sqlite_stmt_is_null(FibSqliteStmt* stmt) {
    return stmt == NULL ? 1 : 0;
}

/* ============================================================================
 * Memory telemetry
 * ============================================================================ */

int64_t fib_sqlite_memory_used(void) {
    return (int64_t)sqlite3_memory_used();
}

int64_t fib_sqlite_memory_highwater(void) {
    return (int64_t)sqlite3_memory_highwater(0);
}

int fib_sqlite_db_cache_used(FibSqliteDb* db) {
    if (!db || !db->db) return 0;
    int cur = 0, hwm = 0;
    if (sqlite3_db_status(db->db, SQLITE_DBSTATUS_CACHE_USED, &cur, &hwm, 0) != SQLITE_OK)
        return 0;
    return cur;
}

int fib_sqlite_db_schema_used(FibSqliteDb* db) {
    if (!db || !db->db) return 0;
    int cur = 0, hwm = 0;
    if (sqlite3_db_status(db->db, SQLITE_DBSTATUS_SCHEMA_USED, &cur, &hwm, 0) != SQLITE_OK)
        return 0;
    return cur;
}

int fib_sqlite_db_stmt_used(FibSqliteDb* db) {
    if (!db || !db->db) return 0;
    int cur = 0, hwm = 0;
    if (sqlite3_db_status(db->db, SQLITE_DBSTATUS_STMT_USED, &cur, &hwm, 0) != SQLITE_OK)
        return 0;
    return cur;
}

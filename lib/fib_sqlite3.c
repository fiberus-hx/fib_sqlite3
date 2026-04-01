/*
 * fib_sqlite3.c - SQLite3 bindings for Fiberus
 *
 * Part of the fib_sqlite3 library (libraries/fib_sqlite3).
 * Implements the C wrapper functions declared in fib_sqlite3.h.
 * All memory returned to Haxe land (FibString, FibBytesData) is
 * GC-managed. SQLite's own allocations (sqlite3*, sqlite3_stmt*)
 * are malloc-based and manually freed.
 */

#include "fib_sqlite3.h"
#include "sqlite_vfs.h"
#include "fibstring.h"
#include "bytes.h"

#include "sqlite3.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* VFS registration state - lazily initialized on first open */
static int fib_sqlite_vfs_initialized = 0;

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

struct FibSqliteDb {
    sqlite3* db;
};

struct FibSqliteStmt {
    sqlite3_stmt* stmt;
    FibSqliteDb*  db;    /* Back-pointer to owning connection (for errmsg) */
};

/* ============================================================================
 * Database Connection
 * ============================================================================ */

FibSqliteDb* fib_sqlite_open(const char* path) {
    if (!path) return NULL;

    /* Lazily register the Fiberus io_uring VFS on first open */
    if (!fib_sqlite_vfs_initialized) {
        fib_sqlite_vfs_register();
        fib_sqlite_vfs_initialized = 1;
    }

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
    if (!stmt || !stmt->stmt) return -1;
    return sqlite3_reset(stmt->stmt) == SQLITE_OK ? 0 : -1;
}

void fib_sqlite_finalize(FibSqliteStmt* stmt) {
    if (!stmt) return;
    if (stmt->stmt) {
        sqlite3_finalize(stmt->stmt);
        stmt->stmt = NULL;
    }
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

    /* Lazily register the Fiberus io_uring VFS on first open */
    if (!fib_sqlite_vfs_initialized) {
        fib_sqlite_vfs_register();
        fib_sqlite_vfs_initialized = 1;
    }

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
    return fs;
}

/* ============================================================================
 * Null Checks
 * ============================================================================ */

int fib_sqlite_db_is_null(FibSqliteDb* db) {
    return db == NULL ? 1 : 0;
}

int fib_sqlite_stmt_is_null(FibSqliteStmt* stmt) {
    return stmt == NULL ? 1 : 0;
}

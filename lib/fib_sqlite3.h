/*
 * fib_sqlite3.h - SQLite3 bindings for Fiberus
 *
 * Part of the fib_sqlite3 library (libraries/fib_sqlite3).
 * Provides a C wrapper around the SQLite3 amalgamation with opaque handles
 * (FibSqliteDb*, FibSqliteStmt*) and Fiberus types (FibString, FibBytesData)
 * for interop with generated code.
 *
 * SQLite is compiled with SQLITE_THREADSAFE=0 (single-threaded mode)
 * since each fiber uses its own connection.
 */

#ifndef FIB_SQLITE3_H
#define FIB_SQLITE3_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations */
typedef struct FibString FibString;
typedef struct FibBytesData FibBytesData;

/* Opaque handles */
typedef struct FibSqliteDb FibSqliteDb;
typedef struct FibSqliteStmt FibSqliteStmt;

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Database Connection
 * ============================================================================ */

/*
 * Open a database file. Returns NULL on failure.
 * path: UTF-8 path to the database file (":memory:" for in-memory).
 */
FibSqliteDb* fib_sqlite_open(const char* path);

/*
 * Close a database connection and free resources.
 */
void fib_sqlite_close(FibSqliteDb* db);

/*
 * Execute one or more SQL statements (no result set).
 * Returns 0 on success, non-zero on error.
 */
int fib_sqlite_exec(FibSqliteDb* db, const char* sql);

/*
 * Get the last error message. Returns NULL if no error.
 */
FibString* fib_sqlite_errmsg(FibSqliteDb* db);

/*
 * Get the rowid of the last INSERT.
 */
int64_t fib_sqlite_last_insert_rowid(FibSqliteDb* db);

/*
 * Get the number of rows changed by the last INSERT/UPDATE/DELETE.
 */
int fib_sqlite_changes(FibSqliteDb* db);

/*
 * Extended result code of the most recent failed API call on this connection.
 * Distinguishes SQLITE_BUSY_SNAPSHOT (517) from SQLITE_BUSY (5) — required for
 * BEGIN CONCURRENT conflict-retry.
 */
int fib_sqlite_extended_errcode(FibSqliteDb* db);

/* ============================================================================
 * Prepared Statements
 * ============================================================================ */

/*
 * Prepare a SQL statement for execution.
 * Returns NULL on error (use fib_sqlite_errmsg to get the error).
 */
FibSqliteStmt* fib_sqlite_prepare(FibSqliteDb* db, const char* sql);

/*
 * Step a prepared statement. Returns:
 *   1  = SQLITE_ROW (row available)
 *   0  = SQLITE_DONE (no more rows)
 *  -1  = error
 */
int fib_sqlite_step(FibSqliteStmt* stmt);

/*
 * Reset a prepared statement so it can be re-executed.
 * Returns 0 on success.
 */
int fib_sqlite_reset(FibSqliteStmt* stmt);

/*
 * Clear all parameter bindings on a prepared statement (sets all to NULL).
 * Returns 0 on success.
 */
int fib_sqlite_clear_bindings(FibSqliteStmt* stmt);

/*
 * Finalize (destroy) a prepared statement.
 */
void fib_sqlite_finalize(FibSqliteStmt* stmt);

/* ============================================================================
 * Binding Parameters (1-indexed)
 * ============================================================================ */

int fib_sqlite_bind_int(FibSqliteStmt* stmt, int idx, int value);
int fib_sqlite_bind_int64(FibSqliteStmt* stmt, int idx, int64_t value);
int fib_sqlite_bind_double(FibSqliteStmt* stmt, int idx, double value);
int fib_sqlite_bind_text(FibSqliteStmt* stmt, int idx, const char* value);
int fib_sqlite_bind_blob(FibSqliteStmt* stmt, int idx, FibBytesData* data, int len);
int fib_sqlite_bind_null(FibSqliteStmt* stmt, int idx);

/*
 * Get the number of bind parameters in the statement.
 */
int fib_sqlite_bind_parameter_count(FibSqliteStmt* stmt);

/*
 * Get the name of a bind parameter (1-indexed). Returns NULL if unnamed.
 */
FibString* fib_sqlite_bind_parameter_name(FibSqliteStmt* stmt, int idx);

/*
 * Get the index of a named bind parameter. Returns 0 if not found.
 */
int fib_sqlite_bind_parameter_index(FibSqliteStmt* stmt, const char* name);

/* ============================================================================
 * Column Accessors (0-indexed, valid after fib_sqlite_step returns 1)
 * ============================================================================ */

/*
 * Get the number of columns in the result set.
 */
int fib_sqlite_column_count(FibSqliteStmt* stmt);

/*
 * Get the name of a column (0-indexed).
 */
FibString* fib_sqlite_column_name(FibSqliteStmt* stmt, int col);

/*
 * Get the SQLite type of a column value (0-indexed).
 * Returns: 1=INTEGER, 2=FLOAT, 3=TEXT, 4=BLOB, 5=NULL
 */
int fib_sqlite_column_type(FibSqliteStmt* stmt, int col);

int         fib_sqlite_column_int(FibSqliteStmt* stmt, int col);
int64_t     fib_sqlite_column_int64(FibSqliteStmt* stmt, int col);
double      fib_sqlite_column_double(FibSqliteStmt* stmt, int col);
FibString*  fib_sqlite_column_text(FibSqliteStmt* stmt, int col);
int         fib_sqlite_column_bytes(FibSqliteStmt* stmt, int col);

/*
 * Get BLOB column data as FibBytesData*. The returned bytes are a copy.
 * *out_len is set to the blob size.
 */
FibBytesData* fib_sqlite_column_blob(FibSqliteStmt* stmt, int col);

/*
 * Get the subtype of a column value (0-indexed).
 * Returns 0 for normal values, 74 ('J') for JSON.
 * Uses sqlite3_column_value() + sqlite3_value_subtype().
 */
int fib_sqlite_column_subtype(FibSqliteStmt* stmt, int col);

/* ============================================================================
 * GC-Safe Wrappers (accept FibString* to avoid dangling const char*)
 *
 * Use these in Haxe @:native bindings instead of fib_sqlite_open/exec/prepare.
 * The original const char* versions remain available for pure C callers.
 * ============================================================================ */

FibSqliteDb*   fib_sqlite_open_safe(FibString* path);
int            fib_sqlite_exec_safe(FibSqliteDb* db, FibString* sql);
FibSqliteStmt* fib_sqlite_prepare_safe(FibSqliteDb* db, FibString* sql);

/* ============================================================================
 * Null Checks (for @:native extern bindings)
 * ============================================================================ */

/*
 * Check if a database handle is NULL. Returns 1 if null, 0 otherwise.
 */
int fib_sqlite_db_is_null(FibSqliteDb* db);

/*
 * Check if a statement handle is NULL. Returns 1 if null, 0 otherwise.
 */
int fib_sqlite_stmt_is_null(FibSqliteStmt* stmt);

/* ============================================================================
 * Memory telemetry
 * ============================================================================ */

/* Process-wide SQLite heap bytes (sqlite3_memory_used). */
int64_t fib_sqlite_memory_used(void);

/* Process-wide SQLite heap highwater (sqlite3_memory_highwater, no reset). */
int64_t fib_sqlite_memory_highwater(void);

/* Per-connection page cache bytes currently used. */
int fib_sqlite_db_cache_used(FibSqliteDb* db);

/* Per-connection memory used by all schemas. */
int fib_sqlite_db_schema_used(FibSqliteDb* db);

/* Per-connection memory used by all prepared statements. */
int fib_sqlite_db_stmt_used(FibSqliteDb* db);

#ifdef __cplusplus
}
#endif

#endif /* FIB_SQLITE3_H */

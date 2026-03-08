/*
 * fib_sqlite3 - Opaque pointer to a SQLite prepared statement.
 *
 * Maps directly to FibSqliteStmt* in generated C code.
 * SQLite manages the memory for statements; the GC does not track these pointers.
 * Call ResultSet.close() to release.
 */
package sqlite3;

@:native("FibSqliteStmt*") @:coreType @:notNull extern abstract FibSqliteStmtPtr {}

/*
 * fib_sqlite3 - Opaque pointer to a SQLite database connection.
 *
 * Maps directly to FibSqliteDb* in generated C code.
 * SQLite manages the memory for connections; the GC does not track these pointers.
 * Call Connection.close() to release.
 */
package sqlite3;

@:native("FibSqliteDb*") @:coreType @:notNull extern abstract FibSqliteDbPtr {}

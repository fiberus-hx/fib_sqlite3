/*
 * fib_sqlite3 - Native SQLite3 extern for Fiberus
 *
 * Direct @:native bindings to the C API in fib_sqlite3.h.
 * Uses a custom SQLite VFS that routes I/O through io_uring for
 * non-blocking database operations within fibers.
 *
 * Usage: Prefer the high-level sqlite3.Sqlite / sqlite3.Connection /
 * sqlite3.ResultSet classes. Use Sqlite3Native directly only when you
 * need low-level control.
 */
package sqlite3;

@:buildXml('
	<files id="__externs__">
		<compilerflag value="-DSQLITE_THREADSAFE=1"/>
		<compilerflag value="-DSQLITE_OMIT_LOAD_EXTENSION"/>
		<compilerflag value="-DSQLITE_ENABLE_FTS5"/>
		<compilerflag value="-DSQLITE_ENABLE_MATH_FUNCTIONS"/>
		<compilerflag value="-DSQLITE_CORE"/>
	</files>
')
@:include("fib_sqlite3.h")
@:sourceFile("../../lib/fib_sqlite3.c")
@:sourceFile("../../lib/fib_sqlite_mutex.c")
@:sourceFile("../../lib/sqlite_vfs.c")
@:sourceFile("../../lib/sqlite-begin-concurrent-20260628/sqlite3.c")
@:sourceFile("../../lib/sqlite-begin-concurrent-20260628/regexp.c")
extern class Sqlite3Native {
	/* ====================================================================
	 * Database Connection
	 * ==================================================================== */

	/* open/exec/prepare use GC-safe wrappers that accept FibString* directly.
	 * The _safe suffix tells the fiberus codegen to NOT insert fib_string_data()
	 * for String parameters, passing FibString* directly. The C wrappers then
	 * call fib_string_data() + strdup() atomically, preventing GC from
	 * relocating the string between extraction and copy. */
	@:native("fib_sqlite_open_safe")
	static function open(path:String):FibSqliteDbPtr;

	@:native("fib_sqlite_close")
	static function close(db:FibSqliteDbPtr):Void;

	@:native("fib_sqlite_exec_safe")
	static function exec(db:FibSqliteDbPtr, sql:String):Int;

	@:native("fib_sqlite_errmsg")
	static function errmsg(db:FibSqliteDbPtr):String;

	@:native("fib_sqlite_last_insert_rowid")
	static function lastInsertRowid(db:FibSqliteDbPtr):haxe.Int64;

	@:native("fib_sqlite_changes")
	static function changes(db:FibSqliteDbPtr):Int;

	/* Extended result code of the last failed call — distinguishes
	 * SQLITE_BUSY_SNAPSHOT (517) from SQLITE_BUSY (5) for BEGIN CONCURRENT retry. */
	@:native("fib_sqlite_extended_errcode")
	static function extendedErrcode(db:FibSqliteDbPtr):Int;

	/* ====================================================================
	 * Prepared Statements
	 * ==================================================================== */

	@:native("fib_sqlite_prepare_safe")
	static function prepare(db:FibSqliteDbPtr, sql:String):FibSqliteStmtPtr;

	@:native("fib_sqlite_step")
	static function step(stmt:FibSqliteStmtPtr):Int;

	@:native("fib_sqlite_reset")
	static function reset(stmt:FibSqliteStmtPtr):Int;

	@:native("fib_sqlite_clear_bindings")
	static function clearBindings(stmt:FibSqliteStmtPtr):Int;

	@:native("fib_sqlite_finalize")
	static function finalize(stmt:FibSqliteStmtPtr):Void;

	/* ====================================================================
	 * Binding Parameters (1-indexed)
	 * ==================================================================== */

	@:native("fib_sqlite_bind_int")
	static function bindInt(stmt:FibSqliteStmtPtr, idx:Int, value:Int):Int;

	@:native("fib_sqlite_bind_int64")
	static function bindInt64(stmt:FibSqliteStmtPtr, idx:Int, value:haxe.Int64):Int;

	@:native("fib_sqlite_bind_double")
	static function bindDouble(stmt:FibSqliteStmtPtr, idx:Int, value:Float):Int;

	@:native("fib_sqlite_bind_text")
	static function bindText(stmt:FibSqliteStmtPtr, idx:Int, value:String):Int;

	@:native("fib_sqlite_bind_blob")
	static function bindBlob(stmt:FibSqliteStmtPtr, idx:Int, data:haxe.io.BytesData, len:Int):Int;

	@:native("fib_sqlite_bind_null")
	static function bindNull(stmt:FibSqliteStmtPtr, idx:Int):Int;

	@:native("fib_sqlite_bind_parameter_count")
	static function bindParameterCount(stmt:FibSqliteStmtPtr):Int;

	@:native("fib_sqlite_bind_parameter_name")
	static function bindParameterName(stmt:FibSqliteStmtPtr, idx:Int):String;

	@:native("fib_sqlite_bind_parameter_index")
	static function bindParameterIndex(stmt:FibSqliteStmtPtr, name:String):Int;

	/* ====================================================================
	 * Column Accessors (0-indexed)
	 * ==================================================================== */

	@:native("fib_sqlite_column_count")
	static function columnCount(stmt:FibSqliteStmtPtr):Int;

	@:native("fib_sqlite_column_name")
	static function columnName(stmt:FibSqliteStmtPtr, col:Int):String;

	@:native("fib_sqlite_column_type")
	static function columnType(stmt:FibSqliteStmtPtr, col:Int):Int;

	@:native("fib_sqlite_column_int")
	static function columnInt(stmt:FibSqliteStmtPtr, col:Int):Int;

	@:native("fib_sqlite_column_int64")
	static function columnInt64(stmt:FibSqliteStmtPtr, col:Int):haxe.Int64;

	@:native("fib_sqlite_column_double")
	static function columnDouble(stmt:FibSqliteStmtPtr, col:Int):Float;

	@:native("fib_sqlite_column_text")
	static function columnText(stmt:FibSqliteStmtPtr, col:Int):String;

	@:native("fib_sqlite_column_bytes")
	static function columnBytes(stmt:FibSqliteStmtPtr, col:Int):Int;

	@:native("fib_sqlite_column_blob")
	static function columnBlob(stmt:FibSqliteStmtPtr, col:Int):haxe.io.BytesData;

	@:native("fib_sqlite_column_subtype")
	static function columnSubtype(stmt:FibSqliteStmtPtr, col:Int):Int;

	/* ====================================================================
	 * Null Checks
	 * ==================================================================== */

	@:native("fib_sqlite_db_is_null")
	static function dbIsNull(db:FibSqliteDbPtr):Int;

	@:native("fib_sqlite_stmt_is_null")
	static function stmtIsNull(stmt:FibSqliteStmtPtr):Int;

	/* ====================================================================
	 * Memory telemetry
	 * ==================================================================== */

	@:native("fib_sqlite_memory_used")
	static function memoryUsed():haxe.Int64;

	@:native("fib_sqlite_memory_highwater")
	static function memoryHighwater():haxe.Int64;

	@:native("fib_sqlite_db_cache_used")
	static function dbCacheUsed(db:FibSqliteDbPtr):Int;

	@:native("fib_sqlite_db_schema_used")
	static function dbSchemaUsed(db:FibSqliteDbPtr):Int;

	@:native("fib_sqlite_db_stmt_used")
	static function dbStmtUsed(db:FibSqliteDbPtr):Int;
}

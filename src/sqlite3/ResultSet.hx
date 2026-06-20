/*
 * fib_sqlite3 - Prepared statement + result iteration
 *
 * Wraps FibSqliteStmtPtr via Sqlite3Native @:native extern calls. Supports:
 * - Parameter binding (by index, 1-based)
 * - Row iteration via step()
 * - Column value access (by index, 0-based)
 * - Reset for re-execution with new bindings
 *
 * Column type constants match SQLite:
 *   1=INTEGER, 2=FLOAT, 3=TEXT, 4=BLOB, 5=NULL
 */
package sqlite3;

class ResultSet {
	var stmt:FibSqliteStmtPtr;

	/* SQLite column type constants */
	public static inline var SQLITE_INTEGER:Int = 1;
	public static inline var SQLITE_FLOAT:Int = 2;
	public static inline var SQLITE_TEXT:Int = 3;
	public static inline var SQLITE_BLOB:Int = 4;
	public static inline var SQLITE_NULL:Int = 5;

	/** JSON subtype marker (74 = ASCII 'J'). Set on values returned by json_*() functions. */
	public static inline var JSON_SUBTYPE:Int = 74;

	/**
	 * Prepare a SQL statement on the given database connection.
	 * Throws on error.
	 */
	public function new(db:FibSqliteDbPtr, sql:String) {
		stmt = Sqlite3Native.prepare(db, sql);
		if (Sqlite3Native.stmtIsNull(stmt) != 0) {
			var errmsg:String = Sqlite3Native.errmsg(db);
			throw "SQLite prepare error: " + (errmsg != null ? errmsg : "unknown");
		}
	}

	/* ====================================================================
	 * Parameter Binding (1-indexed)
	 * ==================================================================== */

	/**
	 * Bind an integer value to parameter at index (1-based).
	 */
	public function bindInt(idx:Int, value:Int):Void {
		var rc:Int = Sqlite3Native.bindInt(stmt, idx, value);
		if (rc != 0)
			throw "SQLite bind_int failed";
	}

	/**
	 * Bind a 64-bit integer value to parameter at index (1-based).
	 */
	public function bindInt64(idx:Int, value:haxe.Int64):Void {
		var rc:Int = Sqlite3Native.bindInt64(stmt, idx, value);
		if (rc != 0)
			throw "SQLite bind_int64 failed";
	}

	/**
	 * Bind a float value to parameter at index (1-based).
	 */
	public function bindFloat(idx:Int, value:Float):Void {
		var rc:Int = Sqlite3Native.bindDouble(stmt, idx, value);
		if (rc != 0)
			throw "SQLite bind_double failed";
	}

	/**
	 * Bind a string value to parameter at index (1-based).
	 * Passing null binds SQL NULL.
	 */
	public function bindText(idx:Int, value:String):Void {
		var rc:Int = Sqlite3Native.bindText(stmt, idx, value);
		if (rc != 0)
			throw "SQLite bind_text failed";
	}

	/**
	 * Bind a blob value to parameter at index (1-based).
	 */
	public function bindBlob(idx:Int, data:haxe.io.Bytes):Void {
		var bd = data.getData();
		var len = data.length;
		var rc:Int = Sqlite3Native.bindBlob(stmt, idx, bd, len);
		if (rc != 0)
			throw "SQLite bind_blob failed";
	}

	/**
	 * Bind NULL to parameter at index (1-based).
	 */
	public function bindNull(idx:Int):Void {
		var rc:Int = Sqlite3Native.bindNull(stmt, idx);
		if (rc != 0)
			throw "SQLite bind_null failed";
	}

	/**
	 * Get the number of bind parameters in the statement.
	 */
	public function parameterCount():Int {
		return Sqlite3Native.bindParameterCount(stmt);
	}

	/**
	 * Get the name of a bind parameter (1-indexed). Returns null if unnamed.
	 */
	public function parameterName(idx:Int):String {
		return Sqlite3Native.bindParameterName(stmt, idx);
	}

	/**
	 * Get the index of a named bind parameter. Returns 0 if not found.
	 */
	public function parameterIndex(name:String):Int {
		return Sqlite3Native.bindParameterIndex(stmt, name);
	}

	/* ====================================================================
	 * Row Iteration
	 * ==================================================================== */

	/**
	 * Step the statement. Returns true if a row is available, false if done.
	 * Throws on error.
	 */
	public function step():Bool {
		var rc:Int = Sqlite3Native.step(stmt);
		if (rc == 1)
			return true;
		if (rc == 0)
			return false;
		throw "SQLite step error";
	}

	/**
	 * Reset the statement for re-execution (e.g. with new bindings).
	 */
	public function reset():Void {
		var rc:Int = Sqlite3Native.reset(stmt);
		if (rc != 0)
			throw "SQLite reset error";
	}

	/**
	 * Clear all parameter bindings (sets all to NULL).
	 * Call after reset() before re-binding for cached statement reuse.
	 */
	public function clearBindings():Void {
		var rc:Int = Sqlite3Native.clearBindings(stmt);
		if (rc != 0)
			throw "SQLite clear_bindings error";
	}

	/**
	 * Finalize and release the statement. Must not be used after this.
	 *
	 * The FibSqliteStmt wrapper is malloc'd (NOT GC-tracked) and owns the
	 * underlying sqlite3_stmt. You MUST call close() before dropping the
	 * last reference to this ResultSet, otherwise both the wrapper and
	 * the SQLite statement leak permanently.
	 *
	 * ConnectionPool.cachedPrepare handles this automatically: cached
	 * statements are evicted via close() on cache overflow and on pool
	 * destroy(). queryDynamic also closes explicitly after iteration.
	 */
	public function close():Void {
		Sqlite3Native.finalize(stmt);
	}

	/* ====================================================================
	 * Column Accessors (0-indexed, valid after step() returns true)
	 * ==================================================================== */

	/**
	 * Get the number of columns in the result set.
	 */
	public function columnCount():Int {
		return Sqlite3Native.columnCount(stmt);
	}

	/**
	 * Get the name of a column (0-indexed).
	 */
	public function columnName(col:Int):String {
		return Sqlite3Native.columnName(stmt, col);
	}

	/**
	 * Get the SQLite type of a column value.
	 * Returns one of: SQLITE_INTEGER(1), SQLITE_FLOAT(2), SQLITE_TEXT(3),
	 *                  SQLITE_BLOB(4), SQLITE_NULL(5)
	 */
	public function columnType(col:Int):Int {
		return Sqlite3Native.columnType(stmt, col);
	}

	/**
	 * Get an integer column value (0-indexed).
	 */
	public function columnInt(col:Int):Int {
		return Sqlite3Native.columnInt(stmt, col);
	}

	/**
	 * Get a 64-bit integer column value (0-indexed).
	 */
	public function columnInt64(col:Int):haxe.Int64 {
		return Sqlite3Native.columnInt64(stmt, col);
	}

	/**
	 * Get a float column value (0-indexed).
	 */
	public function columnFloat(col:Int):Float {
		return Sqlite3Native.columnDouble(stmt, col);
	}

	/**
	 * Get a text column value (0-indexed). Returns null for SQL NULL.
	 */
	public function columnText(col:Int):String {
		return Sqlite3Native.columnText(stmt, col);
	}

	/**
	 * Get the byte size of a column value (0-indexed).
	 * Useful for BLOB columns.
	 */
	public function columnBytes(col:Int):Int {
		return Sqlite3Native.columnBytes(stmt, col);
	}

	/**
	 * Get a BLOB column value as Bytes (0-indexed). Returns null for SQL NULL.
	 */
	public function columnBlob(col:Int):haxe.io.Bytes {
		var bd:haxe.io.BytesData = Sqlite3Native.columnBlob(stmt, col);
		if (bd == null)
			return null;
		return haxe.io.Bytes.ofData(bd);
	}

	/**
	 * Get the subtype of a column value (0-indexed).
	 * Returns 0 for normal values, JSON_SUBTYPE (74) for JSON.
	 * Use this to detect results from json(), json_array(),
	 * json_group_array(), json_object(), -> operator, etc.
	 */
	public function columnSubtype(col:Int):Int {
		return Sqlite3Native.columnSubtype(stmt, col);
	}
}

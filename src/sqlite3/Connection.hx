/*
 * fib_sqlite3 - SQLite database connection
 *
 * Wraps FibSqliteDbPtr via Sqlite3Native @:native extern calls.
 * Supports exec (DDL/DML), prepare (parameterized queries), and
 * metadata (lastInsertRowId, changes, errorMessage).
 */
package sqlite3;

class Connection {
	var db:FibSqliteDbPtr;

	/**
	 * Open a SQLite database connection.
	 * Use ":memory:" for an in-memory database.
	 * Throws on failure.
	 */
	public function new(path:String) {
		db = Sqlite3Native.open(path);
		if (Sqlite3Native.dbIsNull(db) != 0)
			throw "Failed to open SQLite database: " + path;
	}

	/**
	 * Execute one or more SQL statements that don't return rows.
	 * Throws on error.
	 */
	public function exec(sql:String):Void {
		var rc:Int = Sqlite3Native.exec(db, sql);
		if (rc != 0)
			throw "SQLite exec error: " + errorMessage();
	}

	/**
	 * Prepare a SQL statement for execution with parameters.
	 * Returns a ResultSet which can be used to bind parameters and iterate rows.
	 * Throws on error.
	 */
	public function prepare(sql:String):ResultSet {
		return new ResultSet(db, sql);
	}

	/**
	 * Get the rowid of the last inserted row.
	 */
	public function lastInsertRowId():haxe.Int64 {
		return Sqlite3Native.lastInsertRowid(db);
	}

	/**
	 * Get the number of rows changed by the last INSERT/UPDATE/DELETE.
	 */
	public function changes():Int {
		return Sqlite3Native.changes(db);
	}

	/**
	 * Get the last error message, or null if none.
	 */
	public function errorMessage():String {
		return Sqlite3Native.errmsg(db);
	}

	/**
	 * Close the database connection. The connection must not be used after this.
	 */
	public function close():Void {
		Sqlite3Native.close(db);
	}
}

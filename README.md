# fib_sqlite3

SQLite3 database library for the Fiberus runtime. Provides Haxe bindings to the SQLite 3.54.0 (begin-concurrent branch) amalgamation with a custom VFS that routes file I/O through io_uring, allowing fibers to suspend instead of blocking OS threads during database operations.

## Architecture

```
Haxe code -> sqlite3.Connection / sqlite3.ResultSet
          -> sqlite3.Sqlite3Native (@:native extern)
          -> fib_sqlite3.c (C wrapper with opaque handles)
          -> sqlite_vfs.c (custom "fiberus" VFS)
          -> io_uring (xRead/xWrite/xSync are non-blocking)
           -> SQLite 3.54.0 begin-concurrent amalgamation (SQLITE_THREADSAFE=1)
```

The custom VFS wraps the default Unix VFS, replacing only the performance-critical I/O methods (read, write, sync) with io_uring equivalents via `fib_io_pread_raw`, `fib_io_pwrite_raw`, and `fib_io_fsync`. File open/close/lock/stat remain synchronous as they are fast kernel operations. SHM methods for WAL mode are delegated to the Unix VFS.

SQLite is compiled with `SQLITE_THREADSAFE=1` plus a custom fiber-aware mutex backend (`lib/fib_sqlite_mutex.c`), so pooled connections are safe to use across fibers. The amalgamation is built from the **begin-concurrent** branch, enabling `BEGIN CONCURRENT` multi-writer transactions (see `ConnectionPool.transaction`). JSON functions (`json()`, `json_array()`, `json_group_array()`, etc.) and JSONB are fully supported.

## Directory Structure

```
fib_sqlite3/
  lib/
    fib_sqlite3.c          C wrapper (opaque FibSqliteDb*/FibSqliteStmt* handles)
    fib_sqlite3.h          Public C API
    sqlite_vfs.c           Custom "fiberus" VFS (io_uring integration)
    sqlite_vfs.h           VFS registration header
    sqlite-begin-concurrent-20260628/  Vendored begin-concurrent amalgamation (sqlite3.c/.h/ext.h + FOSSIL_HASH); rebuild via lib/vendor-sqlite-bc.sh
  src/sqlite3/
    Sqlite.hx              Factory class (Sqlite.open)
    Connection.hx           Database connection (exec, prepare, close, lastInsertRowId, changes)
    ResultSet.hx            Prepared statement + result iteration (bind*, step, column*, reset, close)
    Sqlite3Native.hx        @:native extern bindings to fib_sqlite3.h
    FibSqliteDbPtr.hx       Opaque pointer type for database connections
    FibSqliteStmtPtr.hx     Opaque pointer type for prepared statements
  test/
    TestMain.hx             Test entry point
    TestSqlite.hx           Test suite (15 test cases)
    Test.hx                 Base test class
    TestFramework.hx        Lightweight assertion framework
    compile.hxml            Haxe compiler flags
    Makefile                Build targets
```

## Usage

```haxe
import sqlite3.Sqlite;

// Open a database (":memory:" for in-memory)
var db = Sqlite.open("mydb.sqlite");

// Execute DDL/DML
db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)");
db.exec("INSERT INTO users (name, age) VALUES ('Alice', 30)");

// Prepared statements with parameter binding (1-indexed)
var ins = db.prepare("INSERT INTO users (name, age) VALUES (?, ?)");
ins.bindText(1, "Bob");
ins.bindInt(2, 25);
ins.step();
ins.reset();

ins.bindText(1, "Carol");
ins.bindInt(2, 28);
ins.step();
ins.close();

// Query with result iteration (columns are 0-indexed)
var rs = db.prepare("SELECT id, name, age FROM users ORDER BY id");
while (rs.step()) {
    trace(rs.columnInt(0) + ": " + rs.columnText(1) + ", age " + rs.columnInt(2));
}
rs.close();

// Metadata
trace("Last insert rowid: " + db.lastInsertRowId());
trace("Rows changed: " + db.changes());

db.close();
```

## API

### sqlite3.Sqlite

| Method | Description |
|--------|-------------|
| `open(path:String):Connection` | Open a database file. Use `":memory:"` for in-memory. Throws on failure. |

### sqlite3.Connection

| Method | Description |
|--------|-------------|
| `exec(sql:String):Void` | Execute SQL statements (no result set). Throws on error. |
| `prepare(sql:String):ResultSet` | Prepare a statement for execution. Throws on error. |
| `lastInsertRowId():haxe.Int64` | Rowid of the last INSERT. |
| `changes():Int` | Rows changed by the last INSERT/UPDATE/DELETE. |
| `errorMessage():String` | Last error message, or null. |
| `close():Void` | Close the connection. |

### sqlite3.ResultSet

**Parameter binding** (1-indexed):

| Method | Description |
|--------|-------------|
| `bindInt(idx, value)` | Bind an Int. |
| `bindInt64(idx, value)` | Bind a haxe.Int64. |
| `bindFloat(idx, value)` | Bind a Float. |
| `bindText(idx, value)` | Bind a String (null binds SQL NULL). |
| `bindBlob(idx, data)` | Bind haxe.io.Bytes as BLOB. |
| `bindNull(idx)` | Bind SQL NULL. |
| `parameterCount():Int` | Number of bind parameters. |
| `parameterName(idx):String` | Name of a bind parameter (null if unnamed). |
| `parameterIndex(name):Int` | Index of a named parameter (0 if not found). |

**Row iteration**:

| Method | Description |
|--------|-------------|
| `step():Bool` | Advance to next row. Returns true if a row is available. Throws on error. |
| `reset():Void` | Reset for re-execution with new bindings. |
| `close():Void` | Finalize the statement. |

**Column accessors** (0-indexed, valid after `step()` returns true):

| Method | Description |
|--------|-------------|
| `columnCount():Int` | Number of result columns. |
| `columnName(col):String` | Column name. |
| `columnType(col):Int` | SQLite type (1=INTEGER, 2=FLOAT, 3=TEXT, 4=BLOB, 5=NULL). |
| `columnInt(col):Int` | Integer value. |
| `columnInt64(col):haxe.Int64` | 64-bit integer value. |
| `columnFloat(col):Float` | Float value. |
| `columnText(col):String` | Text value (null for SQL NULL). |
| `columnBytes(col):Int` | Byte size of value. |
| `columnBlob(col):haxe.io.Bytes` | BLOB value as Bytes (null for SQL NULL). |
| `columnSubtype(col):Int` | Value subtype (0=normal, 74=JSON). Detects `json_*()` function results. |

## Building and Testing

```bash
# From libraries/fib_sqlite3/test/

make debug          # Debug build with FIBERUS_DEBUG
make release        # Release build
make run            # Run tests
make clean          # Remove build artifacts
make cache_clear    # Clear cached .a files
```

The test suite covers: open/close, table creation, insert/select, multiple rows, parameter binding, column types, null values, last insert rowid, changes count, prepared statement reuse, blob data, error handling, named parameters, float64 precision, and column names.

## Memory Model

- **SQLite handles** (`FibSqliteDb*`, `FibSqliteStmt*`) are malloc-allocated and manually freed via `close()` / `finalize()`. The GC does not track these pointers.
- **Return values** to Haxe (FibString, FibBytesData) are GC-managed allocations created by the C wrapper. BLOB column data is copied into a GC-allocated `FibBytesData`.
- The VFS is lazily registered on the first `Sqlite.open()` call.

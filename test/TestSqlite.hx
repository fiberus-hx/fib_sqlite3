package;

import sqlite3.Sqlite;
import sqlite3.Connection;
import sqlite3.ResultSet;

class TestSqlite extends Test {
	public function runTests() {
		testOpenClose();
		testCreateTable();
		testInsertAndSelect();
		testMultipleRows();
		testBindParameters();
		testColumnTypes();
		testNullValues();
		testLastInsertRowId();
		testChanges();
		testPreparedStatementReuse();
		testBlobData();
		testErrorHandling();
		testNamedParameters();
		testFloat64Precision();
		testColumnNames();
		testFts5();
		testRegexpAndMath();
	}

	function testOpenClose() {
		framework.startTest("testOpenClose");
		var db = Sqlite.open(":memory:");
		notNull(db);
		db.close();
		noAssert();
	}

	function testCreateTable() {
		framework.startTest("testCreateTable");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT, value REAL)");
		noAssert(); // No exception = success
		db.close();
	}

	function testInsertAndSelect() {
		framework.startTest("testInsertAndSelect");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)");
		db.exec("INSERT INTO test (name) VALUES ('hello')");
		db.exec("INSERT INTO test (name) VALUES ('world')");

		var rs = db.prepare("SELECT id, name FROM test ORDER BY id");
		t(rs.step());
		eq(1, rs.columnInt(0));
		eqStr("hello", rs.columnText(1));

		t(rs.step());
		eq(2, rs.columnInt(0));
		eqStr("world", rs.columnText(1));

		f(rs.step()); // No more rows
		rs.close();
		db.close();
	}

	function testMultipleRows() {
		framework.startTest("testMultipleRows");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE nums (n INTEGER)");
		for (i in 0...10) {
			db.exec("INSERT INTO nums VALUES (" + i + ")");
		}

		var rs = db.prepare("SELECT n FROM nums ORDER BY n");
		var count = 0;
		while (rs.step()) {
			eq(count, rs.columnInt(0));
			count++;
		}
		eq(10, count);
		rs.close();
		db.close();
	}

	function testBindParameters() {
		framework.startTest("testBindParameters");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, price REAL)");

		var rs = db.prepare("INSERT INTO items (name, price) VALUES (?, ?)");
		rs.bindText(1, "apple");
		rs.bindFloat(2, 1.50);
		rs.step();
		rs.reset();

		rs.bindText(1, "banana");
		rs.bindFloat(2, 0.75);
		rs.step();
		rs.close();

		var sel = db.prepare("SELECT name, price FROM items ORDER BY name");
		t(sel.step());
		eqStr("apple", sel.columnText(0));
		feq(1.50, sel.columnFloat(1));

		t(sel.step());
		eqStr("banana", sel.columnText(1 - 1)); // col 0
		feq(0.75, sel.columnFloat(1));

		f(sel.step());
		sel.close();
		db.close();
	}

	function testColumnTypes() {
		framework.startTest("testColumnTypes");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE types (i INTEGER, f REAL, t TEXT, b BLOB, n)");
		db.exec("INSERT INTO types VALUES (42, 3.14, 'hello', X'DEADBEEF', NULL)");

		var rs = db.prepare("SELECT i, f, t, b, n FROM types");
		t(rs.step());

		eq(ResultSet.SQLITE_INTEGER, rs.columnType(0));
		eq(ResultSet.SQLITE_FLOAT, rs.columnType(1));
		eq(ResultSet.SQLITE_TEXT, rs.columnType(2));
		eq(ResultSet.SQLITE_BLOB, rs.columnType(3));
		eq(ResultSet.SQLITE_NULL, rs.columnType(4));

		eq(42, rs.columnInt(0));
		feq(3.14, rs.columnFloat(1));
		eqStr("hello", rs.columnText(2));

		rs.close();
		db.close();
	}

	function testNullValues() {
		framework.startTest("testNullValues");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE nullable (id INTEGER, val TEXT)");

		var rs = db.prepare("INSERT INTO nullable VALUES (?, ?)");
		rs.bindInt(1, 1);
		rs.bindNull(2);
		rs.step();
		rs.close();

		var sel = db.prepare("SELECT val FROM nullable WHERE id = 1");
		t(sel.step());
		eq(ResultSet.SQLITE_NULL, sel.columnType(0));
		// columnText on NULL should return null
		var val = sel.columnText(0);
		t(val == null);
		sel.close();
		db.close();
	}

	function testLastInsertRowId() {
		framework.startTest("testLastInsertRowId");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE auto (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)");
		db.exec("INSERT INTO auto (name) VALUES ('first')");

		var rowid = db.lastInsertRowId();
		// Compare as Int64
		t(rowid == haxe.Int64.ofInt(1));

		db.exec("INSERT INTO auto (name) VALUES ('second')");
		var rowid2 = db.lastInsertRowId();
		t(rowid2 == haxe.Int64.ofInt(2));

		db.close();
	}

	function testChanges() {
		framework.startTest("testChanges");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE chg (id INTEGER, val INTEGER)");
		db.exec("INSERT INTO chg VALUES (1, 10)");
		db.exec("INSERT INTO chg VALUES (2, 20)");
		db.exec("INSERT INTO chg VALUES (3, 30)");

		db.exec("UPDATE chg SET val = val + 1");
		eq(3, db.changes());

		db.exec("DELETE FROM chg WHERE id > 1");
		eq(2, db.changes());

		db.close();
	}

	function testPreparedStatementReuse() {
		framework.startTest("testPreparedStatementReuse");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE reuse (key TEXT, val INTEGER)");

		var ins = db.prepare("INSERT INTO reuse VALUES (?, ?)");
		var keys = ["a", "b", "c", "d", "e"];
		for (i in 0...keys.length) {
			ins.bindText(1, keys[i]);
			ins.bindInt(2, i * 10);
			ins.step();
			ins.reset();
		}
		ins.close();

		var sel = db.prepare("SELECT val FROM reuse WHERE key = ?");

		sel.bindText(1, "c");
		t(sel.step());
		eq(20, sel.columnInt(0));
		sel.reset();

		sel.bindText(1, "e");
		t(sel.step());
		eq(40, sel.columnInt(0));

		sel.close();
		db.close();
	}

	function testBlobData() {
		framework.startTest("testBlobData");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE blobs (id INTEGER, data BLOB)");

		var data = haxe.io.Bytes.alloc(4);
		data.set(0, 0xDE);
		data.set(1, 0xAD);
		data.set(2, 0xBE);
		data.set(3, 0xEF);

		var ins = db.prepare("INSERT INTO blobs VALUES (1, ?)");
		ins.bindBlob(1, data);
		ins.step();
		ins.close();

		var sel = db.prepare("SELECT data FROM blobs WHERE id = 1");
		t(sel.step());
		eq(ResultSet.SQLITE_BLOB, sel.columnType(0));
		eq(4, sel.columnBytes(0));

		var blob = sel.columnBlob(0);
		notNull(blob);
		eq(4, blob.length);
		eq(0xDE, blob.get(0));
		eq(0xAD, blob.get(1));
		eq(0xBE, blob.get(2));
		eq(0xEF, blob.get(3));

		sel.close();
		db.close();
	}

	function testErrorHandling() {
		framework.startTest("testErrorHandling");
		// Test: open invalid path shouldn't crash (may work depending on OS)
		// Test: exec invalid SQL should throw
		var db = Sqlite.open(":memory:");
		var threw = false;
		try {
			db.exec("THIS IS NOT VALID SQL");
		} catch (e:Dynamic) {
			threw = true;
		}
		t(threw);

		// Test: prepare invalid SQL should throw
		threw = false;
		try {
			var rs = db.prepare("SELECT * FROM nonexistent_table");
		} catch (e:Dynamic) {
			threw = true;
		}
		t(threw);

		db.close();
	}

	function testNamedParameters() {
		framework.startTest("testNamedParameters");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE named (id INTEGER, name TEXT)");

		var rs = db.prepare("INSERT INTO named VALUES (:id, :name)");
		eq(2, rs.parameterCount());

		// Named parameter indices
		var idIdx = rs.parameterIndex(":id");
		var nameIdx = rs.parameterIndex(":name");
		t(idIdx > 0);
		t(nameIdx > 0);

		rs.bindInt(idIdx, 42);
		rs.bindText(nameIdx, "test");
		rs.step();
		rs.close();

		var sel = db.prepare("SELECT id, name FROM named");
		t(sel.step());
		eq(42, sel.columnInt(0));
		eqStr("test", sel.columnText(1));
		sel.close();

		db.close();
	}

	function testFloat64Precision() {
		framework.startTest("testFloat64Precision");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE floats (val REAL)");

		var rs = db.prepare("INSERT INTO floats VALUES (?)");
		rs.bindFloat(1, 3.141592653589793);
		rs.step();
		rs.close();

		var sel = db.prepare("SELECT val FROM floats");
		t(sel.step());
		feq(3.141592653589793, sel.columnFloat(0));
		sel.close();

		db.close();
	}

	function testColumnNames() {
		framework.startTest("testColumnNames");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE TABLE colnames (alpha INTEGER, beta TEXT, gamma REAL)");
		db.exec("INSERT INTO colnames VALUES (1, 'x', 2.0)");

		var rs = db.prepare("SELECT alpha, beta, gamma FROM colnames");
		eq(3, rs.columnCount());
		eqStr("alpha", rs.columnName(0));
		eqStr("beta", rs.columnName(1));
		eqStr("gamma", rs.columnName(2));
		rs.close();
		db.close();
	}

	// SPIKE: proves -DSQLITE_ENABLE_FTS5 compiled into the amalgamation and that
	// MATCH + the bm25() ranking aux function work at the raw fib_sqlite3 layer.
	function testFts5() {
		framework.startTest("testFts5");
		var db = Sqlite.open(":memory:");
		db.exec("CREATE VIRTUAL TABLE docs USING fts5(title, body)");
		db.exec("INSERT INTO docs (title, body) VALUES ('Deploy runbook', 'how to deploy the service to production')");
		db.exec("INSERT INTO docs (title, body) VALUES ('Backup policy', 'database backups run nightly')");
		db.exec("INSERT INTO docs (title, body) VALUES ('Deploy rollback', 'rolling back a bad deploy quickly')");

		// MATCH: 'deploy' hits rows 1 and 3, not row 2.
		var rs = db.prepare("SELECT title FROM docs WHERE docs MATCH 'deploy' ORDER BY rank");
		var n = 0;
		while (rs.step())
			n++;
		rs.close();
		eq(2, n);

		// bm25(): ranking fn returns a real score (negative for a match) — proves the aux fn is available.
		var rk = db.prepare("SELECT title, bm25(docs) AS score FROM docs WHERE docs MATCH 'deploy' ORDER BY score");
		t(rk.step());
		var score = rk.columnFloat(1);
		t(score != 0.0);
		rk.close();

		// phrase + column filter: only the body mentions 'production'.
		var ph = db.prepare("SELECT count(*) FROM docs WHERE docs MATCH 'production'");
		t(ph.step());
		eq(1, ph.columnInt(0));
		ph.close();

		db.close();
	}

	// SPIKE: proves the vendored ext/misc/regexp.c auto-extension is registered
	// (regexp()/regexpi() + REGEXP operator) and -DSQLITE_ENABLE_MATH_FUNCTIONS
	// compiled the math functions into the amalgamation.
	function testRegexpAndMath() {
		framework.startTest("testRegexpAndMath");
		var db = Sqlite.open(":memory:");

		// REGEXP operator (routes to regexp(pattern, string)).
		var rs = db.prepare("SELECT 'deploy-42' REGEXP '^deploy-[0-9]+$', 'rollback' REGEXP '^deploy-[0-9]+$'");
		t(rs.step());
		eq(1, rs.columnInt(0));
		eq(0, rs.columnInt(1));
		rs.close();

		// regexpi(): case-insensitive variant.
		var ri = db.prepare("SELECT regexpi('^DEPLOY', 'deploy-42')");
		t(ri.step());
		eq(1, ri.columnInt(0));
		ri.close();

		// Math functions: pow/sqrt/floor/ln.
		var m = db.prepare("SELECT pow(2, 10), sqrt(144.0), floor(3.7), ln(1.0)");
		t(m.step());
		eq(1024, m.columnInt(0));
		t(m.columnFloat(1) == 12.0);
		eq(3, m.columnInt(2));
		t(m.columnFloat(3) == 0.0);
		m.close();

		db.close();
	}
}

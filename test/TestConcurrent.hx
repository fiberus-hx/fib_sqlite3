package;

import sqlite3.ConnectionPool;
import fiberus.Fiber;
import fiberus.Counter;
import haxe.atomic.AtomicInt;

/**
 * Multi-writer concurrency tests for the begin-concurrent ConnectionPool.
 *
 * Uses a real file DB (begin-concurrent/WAL cannot work on ":memory:", where
 * each open is a separate private database). The DB lives under bin/ (gitignored)
 * and is reset (DROP+CREATE) at the start of each test.
 */
class TestConcurrent extends Test {
	static inline var DB = "bin/concurrent_test.db";

	public function runTests() {
		testWalEnabled();
		testBeginConcurrentSyntax();
		testConcurrentIncrements();
	}

	/** Fresh pool on a reset single-counter table. */
	function freshPool(maxConn:Int):ConnectionPool {
		var pool = ConnectionPool.create(DB, {maxConnections: maxConn});
		pool.withConnection(pc -> {
			pc.exec("DROP TABLE IF EXISTS ctr");
			pc.exec("CREATE TABLE ctr (id INTEGER PRIMARY KEY, n INTEGER)");
			pc.exec("INSERT INTO ctr (id, n) VALUES (1, 0)");
			return 0;
		});
		return pool;
	}

	function testWalEnabled() {
		framework.startTest("testWalEnabled");
		var pool = freshPool(2);
		var jm = pool.withConnection(pc -> {
			var rs = pc.prepare("PRAGMA journal_mode");
			rs.step();
			var v = rs.columnText(0);
			rs.close();
			return v;
		});
		eqStr("wal", jm); // openNew() set PRAGMA journal_mode=WAL
		pool.close();
	}

	function testBeginConcurrentSyntax() {
		framework.startTest("testBeginConcurrentSyntax");
		var pool = freshPool(2);
		var ok = true;
		try {
			pool.withConnection(pc -> {
				pc.exec("BEGIN CONCURRENT"); // syntax-errors on a non-begin-concurrent build
				pc.exec("COMMIT");
				return 0;
			});
		} catch (e:Dynamic) {
			ok = false;
		}
		t(ok);
		pool.close();
	}

	/**
	 * K fibers each run N `transaction()` increments of the SAME row. Concurrent
	 * BEGIN CONCURRENT txns touching one page conflict at COMMIT (517) and are
	 * retried; the final value must still be exactly K*N — proving retry
	 * correctness and that no increment is lost or double-applied.
	 */
	function testConcurrentIncrements() {
		framework.startTest("testConcurrentIncrements");
		var K = 8, N = 25;
		var pool = freshPool(K);
		var done = Counter.create(K);
		var errors = new AtomicInt(0);
		for (k in 0...K) {
			Fiber.spawn(_ -> {
				try {
					for (i in 0...N) {
						pool.transaction(pc -> {
							pc.exec("UPDATE ctr SET n = n + 1 WHERE id = 1");
							return 0;
						});
					}
				} catch (e:Dynamic) {
					errors.add(1);
				}
				done.decrement(); // always, so a failure surfaces as errors>0 not a hang
			});
		}
		done.wait(0);
		done.done();
		eq(0, errors.load()); // no fiber threw (e.g. retries exhausted)
		var total = pool.read(pc -> {
			var rs = pc.prepare("SELECT n FROM ctr WHERE id = 1");
			rs.step();
			var v = rs.columnInt(0);
			rs.close();
			return v;
		});
		eq(K * N, total);
		pool.close();
	}
}

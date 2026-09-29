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
		testWalStaysBounded();
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

	/**
	 * With the background checkpointer (wal_autocheckpoint off), steady writes plus steady readers
	 * never let SQLite restart the WAL on its own: it grew by the full write volume. The checkpointer
	 * must restart it (under the write turn) once it passes its threshold (128 MiB).
	 */
	function testWalStaysBounded() {
		framework.startTest("testWalStaysBounded");
		var db = "bin/wal_bound_test.db";
		for (f in [db, db + "-wal", db + "-shm"])
			if (sys.FileSystem.exists(f)) sys.FileSystem.deleteFile(f);
		var pool = ConnectionPool.create(db, {maxConnections: 4, serialWrites: true, checkpointIntervalMs: 20});
		pool.transaction(pc -> {
			pc.exec("CREATE TABLE b (id INTEGER PRIMARY KEY, v BLOB)");
			return 0;
		});
		var WRITERS = 2, COMMITS = 4000; // 8000 commits x 16 pages of 4 KiB ≈ 512 MiB of WAL frames
		var stop = new AtomicInt(0);
		var maxWal = new AtomicInt(0);
		var done = Counter.create(WRITERS + 2);
		Fiber.spawn(_ -> { // steady readers keep the WAL in use
			while (stop.load() == 0) {
				pool.read(pc -> {
					var rs = pc.prepare("SELECT count(*) FROM b");
					rs.step();
					rs.close();
					return 0;
				});
				Fiber.yield(); // a cached read never suspends: let the writers on this carrier run
			}
			done.decrement();
		});
		Fiber.spawn(_ -> { // sample the WAL file size
			while (stop.load() == 0) {
				if (sys.FileSystem.exists(db + "-wal")) { // (FileSystem.stat is not implemented on fiberus)
					var f = sys.io.File.read(db + "-wal", true);
					f.seek(0, SeekEnd);
					var mb = Std.int(f.tell() / (1024 * 1024));
					f.close();
					if (mb > maxWal.load()) maxWal.store(mb);
				}
				fiberus.io.Timer.sleep(5);
			}
			done.decrement();
		});
		var writersLeft = new AtomicInt(WRITERS);
		for (w in 0...WRITERS) {
			Fiber.spawn(_ -> {
				for (i in 0...COMMITS)
					pool.transaction(pc -> {
						pc.exec("INSERT OR REPLACE INTO b (id, v) VALUES (" + ((w * COMMITS + i) % 64) + ", zeroblob(65536))");
						return 0;
					});
				if (writersLeft.sub(1) == 1) stop.store(1);
				done.decrement();
			});
		}
		done.wait(0);
		done.done();
		pool.close();
		t(maxWal.load() < 256); // bounded: threshold (128 MiB) + one interval of writes, not ~512 MiB
		trace('  max WAL ${maxWal.load()} MiB for ~${Std.int(WRITERS * COMMITS * 68 / 1024)} MiB written');
	}
}

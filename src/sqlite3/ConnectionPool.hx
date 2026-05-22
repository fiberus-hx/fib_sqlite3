/*
 * fib_sqlite3 - Fiber-safe SQLite connection pool
 *
 * Manages a pool of reusable SQLite connections. Acquiring from an
 * exhausted pool suspends the calling fiber until a connection is
 * released. Each pooled connection carries its own prepared-statement
 * cache (FIFO, capped) so hot queries skip sqlite3_prepare_v2.
 *
 * Usage:
 *   var pool = ConnectionPool.create("mydb.sqlite", { maxConnections: 8 });
 *   pool.withConnection((conn) -> {
 *       var rs = conn.cachedPrepare("SELECT * FROM t WHERE id = ?");
 *       // bind, step, read ...
 *   });
 *   pool.close();
 *
 * Concurrency model:
 *   - SQLite is compiled with SQLITE_THREADSAFE=1 and our custom
 *     fiber-aware mutex backend (see lib/fib_sqlite_mutex.c). That
 *     means any fiber on any carrier can safely call into sqlite3_*
 *     — SQLite's per-connection mutex serializes, and on contention
 *     it parks the fiber via Counter instead of OS-thread blocking.
 *   - The pool still provides LEASE semantics so that each
 *     PooledConnection's stmt cache is owned by exactly one fiber at
 *     a time. Without the lease, two fibers hitting the same cached
 *     stmt would race on its bind/step/reset state (a logical race
 *     above the SQLite-mutex layer).
 *
 * Internal wait queue: per-acquire Waiter object holding a Counter
 * (for parking) and a handoff:PooledConnection slot. Release writes
 * the handoff field, then Counter.decrement() — a SEQ_CST atomic
 * that acts as a release fence so the woken fiber sees the field.
 */
package sqlite3;

import sqlite3.Connection;
import sqlite3.ResultSet;
import fiberus.Counter;
import fiberus.Fiber;
import haxe.atomic.AtomicInt;

typedef PoolConfig = {
	/** Maximum number of connections in the pool (default: 4). */
	?maxConnections:Int,

	/** Callback invoked on each newly-opened connection for PRAGMA setup etc. */
	?onConnect:Connection -> Void,

	/** Max distinct prepared statements cached per connection (default: 256).
	 *  FIFO eviction — oldest insert is finalized when cap hit. */
	?stmtCacheSize:Int,
};

/**
 * Per-connection memory snapshot (sqlite3_db_status).
 */
typedef ConnectionStats = {
	/** Page cache bytes (SQLITE_DBSTATUS_CACHE_USED). */
	cache:Int,
	/** Schema bytes (SQLITE_DBSTATUS_SCHEMA_USED). */
	schema:Int,
	/** Prepared-statement bytes (SQLITE_DBSTATUS_STMT_USED). */
	stmt:Int,
	/** Live entries in the local stmtCache. */
	cachedStatements:Int,
};

/**
 * Internal waiter — one allocated per blocked acquire().
 * The releasing fiber writes `handoff` then calls signal.decrement();
 * the waiting fiber wakes from signal.wait(0) and reads `handoff`.
 */
private class Waiter {
	public var signal:Counter;
	public var handoff:PooledConnection;
	public function new() {
		signal = Counter.create(1);
		handoff = null;
	}
}

/**
 * A pooled connection wrapping a raw Connection with a statement cache.
 * Obtained via ConnectionPool.acquire(), returned via release().
 */
class PooledConnection {
	/** The underlying raw SQLite connection. */
	public var conn(default, null):Connection;

	/** Cap on distinct cached statements — FIFO evict on overflow. */
	var cacheSize:Int;
	var stmtCache:Map<String, ResultSet>;
	/** Insertion-order queue — head=oldest. O(1) evict. */
	var insertOrder:Array<String>;
	var pool:ConnectionPool;

	/**
	 * Cached stats snapshot — refreshed on every release(). Read by other
	 * fibers via pool.allStats() without touching SQLite APIs directly.
	 */
	public var lastStats(default, null):ConnectionStats;

	public function new(_conn:Connection, _pool:ConnectionPool, _cacheSize:Int) {
		conn = _conn;
		pool = _pool;
		cacheSize = _cacheSize;
		stmtCache = new Map<String, ResultSet>();
		insertOrder = [];
		refreshStats();
	}

	/**
	 * Get a prepared statement from cache, or prepare and cache it.
	 * On cache hit, the statement is reset and bindings cleared for reuse.
	 * The caller must bind parameters and call step().
	 *
	 * Cache is FIFO-evicted at cacheSize — a distinct-SQL flood cannot
	 * leak unboundedly into sqlite3_stmt allocations.
	 */
	public function cachedPrepare(sql:String):ResultSet {
		var cached = stmtCache.get(sql);
		if (cached != null) {
			cached.reset();
			cached.clearBindings();
			return cached;
		}

		if (insertOrder.length >= cacheSize) {
			var oldSql = insertOrder.shift();
			var oldRs = stmtCache.get(oldSql);
			if (oldRs != null) oldRs.close();
			stmtCache.remove(oldSql);
		}

		var rs = conn.prepare(sql);
		stmtCache.set(sql, rs);
		insertOrder.push(sql);
		return rs;
	}

	/**
	 * Refresh the cached stats snapshot. Called only by the fiber that
	 * currently holds the lease (typically from release()).
	 */
	function refreshStats():Void {
		var db = @:privateAccess conn.db;
		lastStats = {
			cache: Sqlite3Native.dbCacheUsed(db),
			schema: Sqlite3Native.dbSchemaUsed(db),
			stmt: Sqlite3Native.dbStmtUsed(db),
			cachedStatements: insertOrder.length,
		};
	}

	/**
	 * Proxy: execute SQL that doesn't return rows.
	 */
	public inline function exec(sql:String):Void {
		conn.exec(sql);
	}

	/**
	 * Proxy: prepare without caching (for dynamic SQL with varying structure).
	 */
	public inline function prepare(sql:String):ResultSet {
		return conn.prepare(sql);
	}

	/**
	 * Proxy: get the last error message.
	 */
	public inline function errorMessage():String {
		return conn.errorMessage();
	}

	/**
	 * Finalize all cached statements and close the underlying connection.
	 * Called by the pool when retiring a connection — do not call directly.
	 */
	public function destroy():Void {
		for (rs in stmtCache) {
			rs.close();
		}
		stmtCache.clear();
		insertOrder = [];
		conn.close();
	}

	/**
	 * Release this connection back to the pool.
	 */
	public function release():Void {
		refreshStats();
		pool.release(this);
	}
}

class ConnectionPool {
	var path:String;
	var config:PoolConfig;
	var closed:Bool;
	/** All pooled connections — used for stats enumeration. */
	var allConnections:Array<PooledConnection>;

	/** Idle conns (LIFO stack — last released is first reused, keeps cache warm). */
	var freeConns:Array<PooledConnection>;
	/** Fibers parked waiting for a conn (FIFO). */
	var waiters:Array<Waiter>;
	/** Spinlock protecting freeConns + waiters (0 = unlocked, 1 = locked). */
	var queueLock:AtomicInt;

	function new(_path:String, _config:PoolConfig) {
		path = _path;
		config = _config;
		closed = false;
		allConnections = [];
		freeConns = [];
		waiters = [];
		queueLock = new AtomicInt(0);
	}

	/**
	 * Create a new connection pool for the given database path.
	 * Pre-warms all connections eagerly so first requests don't pay
	 * connection-open cost and PRAGMAs + page cache are ready.
	 */
	public static function create(dbPath:String, ?config:PoolConfig):ConnectionPool {
		if (config == null) config = {};
		var maxConn = (config.maxConnections ?? 4);

		/* The pool + pooled connections are shared across all fibers.
		 * Allocate in mature space so they're not relocated by minor GC
		 * and have stable addresses for cross-fiber sharing. */
		untyped __fiberus__("gc_force_mature_begin()");
		var pool = new ConnectionPool(dbPath, config);
		for (_ in 0...maxConn) {
			pool.freeConns.push(pool.openNew());
		}
		untyped __fiberus__("gc_force_mature_end()");
		return pool;
	}

	/**
	 * Acquire a connection from the pool.
	 * If all connections are in use, the calling fiber suspends
	 * until a connection is released.
	 */
	public function acquire():PooledConnection {
		if (closed)
			throw "ConnectionPool is closed";

		spinLock();
		if (freeConns.length > 0) {
			var pc = freeConns.pop();
			spinUnlock();
			return pc;
		}
		// Park: allocate Waiter, queue, release lock, wait.
		var w = new Waiter();
		waiters.push(w);
		spinUnlock();
		w.signal.wait(0);
		w.signal.done();
		// release() wrote handoff before decrement(); SEQ_CST gives us visibility.
		if (w.handoff == null)
			throw "ConnectionPool was closed while acquire() was waiting";
		return w.handoff;
	}

	/**
	 * Release a connection back to the pool.
	 * If the pool is closed, the connection is destroyed instead.
	 */
	public function release(pc:PooledConnection):Void {
		if (closed) {
			pc.destroy();
			return;
		}
		spinLock();
		if (waiters.length > 0) {
			// Direct hand-off: pop waiter, write handoff field, wake.
			var w = waiters.shift();
			w.handoff = pc;
			spinUnlock();
			w.signal.decrement();
			return;
		}
		freeConns.push(pc);
		spinUnlock();
	}

	/**
	 * Acquire a connection, run the callback, and release it back.
	 * Guarantees release even if the callback throws.
	 */
	public function withConnection<T>(fn:PooledConnection -> T):T {
		var pc = acquire();
		try {
			var result = fn(pc);
			pc.release();
			return result;
		} catch (e:Dynamic) {
			pc.release();
			throw e;
		}
	}

	/**
	 * Close the pool: drain idle conns, signal pending waiters, destroy all.
	 * Outstanding leased connections will be destroyed when released.
	 */
	public function close():Void {
		spinLock();
		closed = true;
		// Destroy idle conns
		while (freeConns.length > 0) {
			var pc = freeConns.pop();
			pc.destroy();
		}
		// Wake all parked waiters with a null handoff (they will throw).
		while (waiters.length > 0) {
			var w = waiters.shift();
			w.handoff = null;
			w.signal.decrement();
		}
		spinUnlock();
	}

	// --- Internal ---

	function openNew():PooledConnection {
		var conn = Sqlite.open(path);
		if (config.onConnect != null)
			config.onConnect(conn);
		var cacheSize = config.stmtCacheSize ?? 256;
		var pc = new PooledConnection(conn, this, cacheSize);
		allConnections.push(pc);
		return pc;
	}

	function spinLock():Void {
		// Fast path: try CAS once.
		if (queueLock.compareExchange(0, 1) == 0) return;
		// Contended: yield-with-backoff so we don't burn the carrier CPU.
		// Critical section is microseconds (one array push/pop), so a few
		// quick retries should resolve it; otherwise yield to the scheduler.
		var spins = 0;
		while (true) {
			if (queueLock.compareExchange(0, 1) == 0) return;
			spins++;
			if ((spins & 0x7) == 0) {
				Fiber.yield();
			}
		}
	}

	inline function spinUnlock():Void {
		queueLock.store(0);
	}

	/**
	 * Path used to open this pool (for logging/telemetry).
	 */
	public function dbPath():String return path;

	/**
	 * Aggregate per-connection stats using the cached snapshot refreshed
	 * on each release(). Safe to call from any fiber at any time — no
	 * SQLite API calls happen here.
	 * Freshness = "last time the conn was released" (bounded by request latency).
	 */
	public function allStats():Array<ConnectionStats> {
		var out = new Array<ConnectionStats>();
		for (pc in allConnections)
			out.push(pc.lastStats);
		return out;
	}
}

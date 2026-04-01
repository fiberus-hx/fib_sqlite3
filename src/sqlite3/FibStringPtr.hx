/*
 * fib_sqlite3 - Raw FibString pointer type for GC-safe native calls.
 *
 * When String is passed to @:native extern functions, the codegen inserts
 * fib_string_data() to convert FibString* to const char*. This creates a
 * dangling pointer risk: GC can relocate the FibString between the
 * fib_string_data() call and the native function's strdup().
 *
 * FibStringPtr maps directly to FibString* in generated C code WITHOUT
 * the fib_string_data() conversion. Native functions that accept FibStringPtr
 * receive the GC-managed pointer and can safely call fib_string_data() +
 * strdup() in a single atomic statement.
 *
 * Usage: cast String to FibStringPtr via untyped or implicit conversion.
 */
package sqlite3;

@:native("FibString*") @:coreType @:notNull extern abstract FibStringPtr from String {}

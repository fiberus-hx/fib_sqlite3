package;

class TestMain {
	static function main() {
		var framework = new TestFramework();

		trace("--- TestSqlite ---");
		var sqliteTests = new TestSqlite();
		sqliteTests.setFramework(framework);
		sqliteTests.runTests();

		trace("");
		trace("--- TestConcurrent ---");
		var concurrentTests = new TestConcurrent();
		concurrentTests.setFramework(framework);
		concurrentTests.runTests();

		trace("");
		framework.printSummary();
	}
}

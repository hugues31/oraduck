#include "catch.hpp"
#include "oraduck/direct_path_loader.hpp"
#include "itest_support.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace oraduck;

namespace {

const char *kTable = "ITEST_DP";

std::vector<DirPathColumnSpec> Specs() {
	return {{"ID", SQLT_INT, 8, ""}, {"S", SQLT_CHR, 30, ""}};
}

// Loads rows [first, first + count): ID = i, S = "s" + i
void LoadRange(DirectPathLoader &loader, int64_t first, int64_t count) {
	std::vector<int64_t> ids(loader.Rows());
	std::vector<std::string> texts(loader.Rows());
	for (int64_t done = 0; done < count;) {
		const ub4 n = static_cast<ub4>(std::min<int64_t>(loader.Rows(), count - done));
		for (ub4 r = 0; r < n; r++) {
			ids[r] = first + done + r;
			texts[r] = "s" + std::to_string(ids[r]);
			loader.SetCell(r, 0, &ids[r], sizeof(int64_t));
			loader.SetCell(r, 1, texts[r].data(), static_cast<ub4>(texts[r].size()));
		}
		loader.ConvertAndLoad(n);
		done += n;
	}
}

// Expected "count/sum(ID)/sum(LENGTH(S))" for rows [0, n)
std::string Expected(int64_t n) {
	int64_t lengths = 0;
	for (int64_t i = 0; i < n; i++) {
		lengths += 1 + static_cast<int64_t>(std::to_string(i).size());
	}
	return std::to_string(n) + "/" + std::to_string(n * (n - 1) / 2) + "/" + std::to_string(lengths);
}

struct Fixture {
	OciSession session {TestCredentials()};
	Fixture() {
		DropTableIfExists(session, kTable);
		session.Execute("CREATE TABLE ITEST_DP (ID NUMBER(19), S VARCHAR2(30))");
	}
	~Fixture() {
		DropTableIfExists(session, kTable);
	}
	std::string Stats() {
		return Scalar(session,
		              "SELECT COUNT(*) || '/' || NVL(SUM(ID), 0) || '/' || NVL(SUM(LENGTH(S)), 0) FROM ITEST_DP");
	}
};

} // namespace

TEST_CASE("Simple load", "[oracle][dirpath]") {
	Fixture f;
	DirectPathLoader loader(TestCredentials(), "BENCH", kTable, Specs(), LoaderOptions {});
	if (!loader.contiguous_arrays()) {
		WARN("column arrays are not contiguous: using the OCIDirPathColArrayEntrySet path");
	}
	LoadRange(loader, 0, 10000);
	loader.Finish();
	CHECK(f.Stats() == Expected(10000));
	CHECK(loader.rows_loaded() == 10000);
}

TEST_CASE("Small stream buffer (OCI_CONTINUE)", "[oracle][dirpath]") {
	Fixture f;
	LoaderOptions options;
	options.stream_buffer_bytes = 16 * 1024;
	DirectPathLoader loader(TestCredentials(), "BENCH", kTable, Specs(), options);
	LoadRange(loader, 0, 10000);
	loader.Finish();
	CHECK(f.Stats() == Expected(10000));
}

TEST_CASE("Stream sent after each column array", "[oracle][dirpath]") {
	Fixture f;
	LoaderOptions options;
	options.accumulate_stream = false;
	DirectPathLoader loader(TestCredentials(), "BENCH", kTable, Specs(), options);
	LoadRange(loader, 0, 10000);
	loader.Finish();
	CHECK(f.Stats() == Expected(10000));
}

TEST_CASE("EntrySet path", "[oracle][dirpath]") {
	Fixture f;
	LoaderOptions options;
	options.force_entry_set = true;
	DirectPathLoader loader(TestCredentials(), "BENCH", kTable, Specs(), options);
	CHECK_FALSE(loader.contiguous_arrays());
	LoadRange(loader, 0, 5000);
	loader.Finish();
	CHECK(f.Stats() == Expected(5000));
}

TEST_CASE("Explicit and implicit abort", "[oracle][dirpath]") {
	Fixture f;
	{
		DirectPathLoader loader(TestCredentials(), "BENCH", kTable, Specs(), LoaderOptions {});
		LoadRange(loader, 0, 5000);
		loader.Abort();
	}
	{
		DirectPathLoader loader(TestCredentials(), "BENCH", kTable, Specs(), LoaderOptions {});
		LoadRange(loader, 0, 5000);
	} // destructor without Finish
	CHECK(f.Stats() == Expected(0));
}

TEST_CASE("Parallel sessions on the same table", "[oracle][dirpath]") {
	Fixture f;
	constexpr int kSessions = 4;
	constexpr int64_t kPerSession = 5000;
	std::vector<std::unique_ptr<DirectPathLoader>> loaders;
	for (int i = 0; i < kSessions; i++) {
		loaders.push_back(
		    std::make_unique<DirectPathLoader>(TestCredentials(), "BENCH", kTable, Specs(), LoaderOptions {}));
	}
	std::vector<std::string> errors(kSessions);
	std::vector<std::thread> threads;
	for (int i = 0; i < kSessions; i++) {
		threads.emplace_back([&, i] {
			try {
				LoadRange(*loaders[i], i * kPerSession, kPerSession);
			} catch (const std::exception &e) {
				errors[i] = e.what();
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	for (auto &error : errors) {
		REQUIRE(error.empty());
	}
	for (auto &loader : loaders) {
		loader->Finish();
	}
	CHECK(f.Stats() == Expected(kSessions * kPerSession));
}

TEST_CASE("Indexed table: refused in parallel, loaded with index maintenance skipped", "[oracle][dirpath]") {
	Fixture f;
	f.session.Execute("CREATE INDEX ITEST_DP_IX ON ITEST_DP (ID)");
	try {
		DirectPathLoader refused(TestCredentials(), "BENCH", kTable, Specs(), LoaderOptions {});
		FAIL("parallel direct path load accepted an indexed table");
	} catch (const OracleError &e) {
		CHECK(e.ora_code == 26002);
	}

	LoaderOptions options;
	options.skip_index_maintenance = true; // sqlldr skip_index_maintenance=true
	std::vector<std::unique_ptr<DirectPathLoader>> loaders;
	for (int i = 0; i < 2; i++) {
		loaders.push_back(std::make_unique<DirectPathLoader>(TestCredentials(), "BENCH", kTable, Specs(), options));
	}
	LoadRange(*loaders[0], 0, 3000);
	LoadRange(*loaders[1], 3000, 3000);
	for (auto &loader : loaders) {
		loader->Finish();
	}
	CHECK(f.Stats() == Expected(6000));
	CHECK(Scalar(f.session, "SELECT status FROM user_indexes WHERE index_name = 'ITEST_DP_IX'") == "UNUSABLE");
	f.session.Execute("ALTER INDEX ITEST_DP_IX REBUILD");
	CHECK(Scalar(f.session, "SELECT status FROM user_indexes WHERE index_name = 'ITEST_DP_IX'") == "VALID");
}

TEST_CASE("Value too long", "[oracle][dirpath]") {
	Fixture f;
	DirectPathLoader loader(TestCredentials(), "BENCH", kTable, Specs(), LoaderOptions {});
	const std::string big(40, 'x');
	const int64_t id = 1;
	loader.SetCell(0, 0, &id, sizeof(id));
	loader.SetCell(0, 1, big.data(), static_cast<ub4>(big.size()));
	CHECK_THROWS_AS(loader.ConvertAndLoad(1), OraduckError);
	loader.Abort();
	CHECK(f.Stats() == Expected(0));
}

TEST_CASE("Row wider than the stream buffer", "[oracle][dirpath]") {
	OciSession session(TestCredentials());
	DropTableIfExists(session, "ITEST_WIDE");
	std::string ddl = "CREATE TABLE ITEST_WIDE (";
	std::vector<DirPathColumnSpec> specs;
	for (int c = 0; c < 20; c++) {
		const std::string name = "C" + std::to_string(c);
		ddl += (c ? ", " : "") + name + " VARCHAR2(4000)";
		specs.push_back({name, SQLT_CHR, 4000, ""});
	}
	session.Execute(ddl + ")");
	LoaderOptions options;
	options.stream_buffer_bytes = 64 * 1024;
	const std::string value(4000, 'w');
	bool loaded = false;
	try {
		DirectPathLoader loader(TestCredentials(), "BENCH", "ITEST_WIDE", specs, options);
		for (ub4 r = 0; r < 10; r++) {
			for (ub2 c = 0; c < 20; c++) {
				loader.SetCell(r, c, value.data(), static_cast<ub4>(value.size()));
			}
		}
		loader.ConvertAndLoad(10);
		loader.Finish();
		loaded = true;
	} catch (const OraduckError &e) {
		// The only accepted error: the row does not fit in the stream
		CHECK_THAT(e.what(), Catch::Contains("STREAM_SIZE"));
	}
	if (loaded) {
		CHECK(Scalar(session, "SELECT COUNT(*) || '/' || SUM(LENGTH(C0) + LENGTH(C19)) FROM ITEST_WIDE") == "10/80000");
	} else {
		CHECK(Scalar(session, "SELECT COUNT(*) FROM ITEST_WIDE") == "0");
	}
	DropTableIfExists(session, "ITEST_WIDE");
}

TEST_CASE("Mixed-case identifiers", "[oracle][dirpath]") {
	OciSession session(TestCredentials());
	DropTableIfExists(session, "\"ItestMixed\"");
	session.Execute("CREATE TABLE \"ItestMixed\" (\"MaCol\" NUMBER)");
	// Names exactly as stored in the dictionary: the loader quotes them itself
	DirectPathLoader loader(TestCredentials(), "BENCH", "ItestMixed", {{"MaCol", SQLT_INT, 8, ""}}, LoaderOptions {});
	const int64_t v = 42;
	loader.SetCell(0, 0, &v, sizeof(v));
	loader.ConvertAndLoad(1);
	loader.Finish();
	CHECK(Scalar(session, "SELECT SUM(\"MaCol\") FROM \"ItestMixed\"") == "42");
	DropTableIfExists(session, "\"ItestMixed\"");
}

TEST_CASE("Concurrent loader creation", "[oracle][dirpath][concurrency]") {
	// Reproduces the SIGSEGV (ORA-21500 KGHALO4) caused by concurrent OCI allocations on a
	// shared environment: each round opens 8 loaders at the same time.
	Fixture f;
	constexpr int kThreads = 8;
	constexpr int kRounds = 15;
	for (int round = 0; round < kRounds; round++) {
		std::vector<std::string> errors(kThreads);
		std::vector<std::thread> threads;
		for (int i = 0; i < kThreads; i++) {
			threads.emplace_back([&, i] {
				try {
					DirectPathLoader loader(TestCredentials(), "BENCH", kTable, Specs(), LoaderOptions {});
					LoadRange(loader, int64_t(round) * 1000 + i * 100, 100);
					loader.Finish();
				} catch (const std::exception &e) {
					errors[i] = e.what();
				}
			});
		}
		for (auto &thread : threads) {
			thread.join();
		}
		for (auto &error : errors) {
			REQUIRE(error.empty());
		}
	}
	CHECK(Scalar(f.session, "SELECT COUNT(*) FROM ITEST_DP") == std::to_string(kThreads * 100 * kRounds));
}

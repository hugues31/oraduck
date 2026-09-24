// OraDuck probe: OCI Direct Path variants measured on Oracle 19c.
// Usage: source env.sh && make probe | tee docs/probe-results.md
#include "oraduck/direct_path_loader.hpp"
#include "oraduck/oracle_encoding.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace oraduck;

namespace {

constexpr int64_t kRows = 1000000;
constexpr ub4 kMaxArrayRows = 8192;
constexpr ub2 kInternalTimestamp = 180; // internal TIMESTAMP type (not documented for direct path)

OracleCredentials Credentials() {
	auto get = [](const char *name) {
		const char *value = std::getenv(name);
		if (!value || !*value) {
			std::fprintf(stderr, "%s is not set: run 'source env.sh'\n", name);
			std::exit(2);
		}
		return std::string(value);
	};
	return {get("ORADUCK_ORACLE_USER"), get("ORADUCK_ORACLE_PASSWORD"), get("ORADUCK_ORACLE_DSN")};
}

double Seconds(const std::function<void()> &fn) {
	const auto start = std::chrono::steady_clock::now();
	fn();
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::string Scalar(OciSession &s, const std::string &sql) {
	return s.QueryStrings(sql).at(0).at(0);
}

void Recreate(OciSession &s, const std::string &table, const std::string &columns) {
	s.Execute("BEGIN EXECUTE IMMEDIATE 'DROP TABLE " + table +
	          " PURGE'; EXCEPTION WHEN OTHERS THEN IF SQLCODE != -942 THEN RAISE; END IF; END;");
	s.Execute("CREATE TABLE " + table + " (" + columns + ")");
}

void Drop(OciSession &s, const std::string &table) {
	s.Execute("BEGIN EXECUTE IMMEDIATE 'DROP TABLE " + table +
	          " PURGE'; EXCEPTION WHEN OTHERS THEN IF SQLCODE != -942 THEN RAISE; END IF; END;");
}

uint64_t Magnitude(int64_t v) {
	return v < 0 ? uint64_t(0) - uint64_t(v) : uint64_t(v);
}

// Values generated for global index i
int64_t IntValue(int64_t i) {
	return i * 7919 - 3000000;
}
int64_t CentsValue(int64_t i) {
	return (i * 12345) % 10000000 - 5000000;
}
int64_t DayValue(int64_t i) {
	return i % 20000 - 5000;
}
int64_t SecondOfDay(int64_t i) {
	return (i * 37) % 86400;
}
int64_t MicrosOfDay(int64_t i) {
	return (i * 7777777LL) % kMicrosPerDay;
}
double DoubleValue(int64_t i) {
	return static_cast<double>(i) * 0.5 - 1000.0;
}

using RowWriter = std::function<void(DirectPathLoader &, ub4 row, int64_t i)>;

void Feed(DirectPathLoader &loader, int64_t first, int64_t rows, const RowWriter &write) {
	for (int64_t done = 0; done < rows;) {
		const ub4 n = static_cast<ub4>(std::min<int64_t>(loader.Rows(), rows - done));
		for (ub4 r = 0; r < n; r++) {
			write(loader, r, first + done + r);
		}
		loader.ConvertAndLoad(n);
		done += n;
	}
}

struct Variant {
	std::string label;
	std::string table;
	std::string ddl;
	DirPathColumnSpec spec;
	RowWriter write;
	std::string check_sql;
	std::string expected;
	LoaderOptions options;
};

void RunVariant(OciSession &s, const Variant &v) {
	try {
		Recreate(s, v.table, v.ddl);
		const double t = Seconds([&] {
			DirectPathLoader loader(Credentials(), "BENCH", v.table, {v.spec}, v.options);
			Feed(loader, 0, kRows, v.write);
			loader.Finish();
		});
		const std::string got = Scalar(s, v.check_sql);
		const std::string verdict = got == v.expected ? "OK" : "FAILED: got " + got + ", expected " + v.expected;
		std::printf("| %s | %.3f | %.0f | %s |\n", v.label.c_str(), t, kRows / t, verdict.c_str());
	} catch (const std::exception &e) {
		std::printf("| %s | — | — | ERROR: %s |\n", v.label.c_str(), e.what());
	}
	std::fflush(stdout);
}

std::string CountSlash(const std::string &rest) {
	return std::to_string(kRows) + "/" + rest;
}

std::vector<Variant> SingleColumnVariants() {
	std::vector<Variant> out;
	auto ints = std::make_shared<std::vector<int64_t>>(kMaxArrayRows);
	auto doubles = std::make_shared<std::vector<double>>(kMaxArrayRows);
	auto bytes = std::make_shared<std::vector<uint8_t>>(kMaxArrayRows * 32);
	auto texts = std::make_shared<std::vector<std::string>>(kMaxArrayRows);

	// NUMBER <- int64
	int64_t int_sum = 0;
	for (int64_t i = 0; i < kRows; i++) {
		int_sum += IntValue(i);
	}
	const std::string num_check = "SELECT COUNT(*) || '/' || SUM(C) FROM PROBE_NUM";
	const std::string num_expected = CountSlash(std::to_string(int_sum));
	out.push_back({"NUMBER ← int64, SQLT_INT (native)", "PROBE_NUM", "C NUMBER(19)", {"C", SQLT_INT, 8, ""},
	               [ints](DirectPathLoader &l, ub4 r, int64_t i) {
		               (*ints)[r] = IntValue(i);
		               l.SetCell(r, 0, &(*ints)[r], 8);
	               },
	               num_check, num_expected, {}});
	out.push_back({"NUMBER ← int64, SQLT_NUM (encoded)", "PROBE_NUM", "C NUMBER(19)", {"C", SQLT_NUM, 22, ""},
	               [bytes](DirectPathLoader &l, ub4 r, int64_t i) {
		               const int64_t v = IntValue(i);
		               uint8_t *p = bytes->data() + r * 32;
		               l.SetCell(r, 0, p, static_cast<ub4>(EncodeOracleNumber(Magnitude(v), v < 0, 0, p)));
	               },
	               num_check, num_expected, {}});
	out.push_back({"NUMBER ← int64, SQLT_CHR (text)", "PROBE_NUM", "C NUMBER(19)", {"C", SQLT_CHR, 24, ""},
	               [bytes](DirectPathLoader &l, ub4 r, int64_t i) {
		               char *p = reinterpret_cast<char *>(bytes->data() + r * 32);
		               const int n = std::snprintf(p, 24, "%lld", static_cast<long long>(IntValue(i)));
		               l.SetCell(r, 0, p, static_cast<ub4>(n));
	               },
	               num_check, num_expected, {}});
	LoaderOptions slow;
	slow.force_entry_set = true;
	out.push_back({"NUMBER ← int64, SQLT_INT, forced EntrySet", "PROBE_NUM", "C NUMBER(19)", {"C", SQLT_INT, 8, ""},
	               [ints](DirectPathLoader &l, ub4 r, int64_t i) {
		               (*ints)[r] = IntValue(i);
		               l.SetCell(r, 0, &(*ints)[r], 8);
	               },
	               num_check, num_expected, slow});

	// NUMBER(15,2) <- cents
	int64_t cents_sum = 0;
	for (int64_t i = 0; i < kRows; i++) {
		cents_sum += CentsValue(i);
	}
	const std::string dec_check = "SELECT COUNT(*) || '/' || TO_CHAR(SUM(C) * 100) FROM PROBE_DEC";
	const std::string dec_expected = CountSlash(std::to_string(cents_sum));
	out.push_back({"NUMBER(15,2) ← decimal, SQLT_NUM (encoded)", "PROBE_DEC", "C NUMBER(15,2)", {"C", SQLT_NUM, 22, ""},
	               [bytes](DirectPathLoader &l, ub4 r, int64_t i) {
		               const int64_t v = CentsValue(i);
		               uint8_t *p = bytes->data() + r * 32;
		               l.SetCell(r, 0, p, static_cast<ub4>(EncodeOracleNumber(Magnitude(v), v < 0, 2, p)));
	               },
	               dec_check, dec_expected, {}});
	out.push_back({"NUMBER(15,2) ← decimal, SQLT_CHR (text)", "PROBE_DEC", "C NUMBER(15,2)", {"C", SQLT_CHR, 24, ""},
	               [bytes](DirectPathLoader &l, ub4 r, int64_t i) {
		               const int64_t v = CentsValue(i);
		               char *p = reinterpret_cast<char *>(bytes->data() + r * 32);
		               const int n = std::snprintf(p, 24, "%s%llu.%02llu", v < 0 ? "-" : "",
		                                           static_cast<unsigned long long>(Magnitude(v) / 100),
		                                           static_cast<unsigned long long>(Magnitude(v) % 100));
		               l.SetCell(r, 0, p, static_cast<ub4>(n));
	               },
	               dec_check, dec_expected, {}});

	// DATE
	int64_t date_sum = 0;
	for (int64_t i = 0; i < kRows; i++) {
		date_sum += DayValue(i) * 86400 + SecondOfDay(i);
	}
	const std::string date_check =
	    "SELECT COUNT(*) || '/' || TO_CHAR(ROUND(SUM((C - DATE '1970-01-01') * 86400))) FROM PROBE_DATE";
	const std::string date_expected = CountSlash(std::to_string(date_sum));
	out.push_back({"DATE, SQLT_DAT (7 bytes)", "PROBE_DATE", "C DATE", {"C", SQLT_DAT, 7, ""},
	               [bytes](DirectPathLoader &l, ub4 r, int64_t i) {
		               uint8_t *p = bytes->data() + r * 32;
		               EncodeOracleDate(DayValue(i), SecondOfDay(i) * kMicrosPerSecond, p);
		               l.SetCell(r, 0, p, 7);
	               },
	               date_check, date_expected, {}});
	out.push_back({"DATE, SQLT_CHR (text, 19)", "PROBE_DATE", "C DATE", {"C", SQLT_CHR, 19, "YYYY-MM-DD HH24:MI:SS"},
	               [bytes](DirectPathLoader &l, ub4 r, int64_t i) {
		               char *p = reinterpret_cast<char *>(bytes->data() + r * 32);
		               FormatDateTimeText(DayValue(i), SecondOfDay(i) * kMicrosPerSecond, p);
		               l.SetCell(r, 0, p, 19);
	               },
	               date_check, date_expected, {}});

	// TIMESTAMP(6)
	int64_t ts_days = 0, ts_micros = 0;
	for (int64_t i = 0; i < kRows; i++) {
		ts_days += DayValue(i);
		ts_micros += MicrosOfDay(i);
	}
	const std::string ts_check = "SELECT COUNT(*) || '/' || SUM(TRUNC(C) - DATE '1970-01-01') || '/' || "
	                             "SUM(TO_NUMBER(TO_CHAR(C, 'SSSSS')) * 1000000 + TO_NUMBER(TO_CHAR(C, 'FF6'))) "
	                             "FROM PROBE_TS";
	const std::string ts_expected = CountSlash(std::to_string(ts_days) + "/" + std::to_string(ts_micros));
	out.push_back({"TIMESTAMP(6), SQLT_CHR (text, 26)", "PROBE_TS", "C TIMESTAMP(6)",
	               {"C", SQLT_CHR, 26, "YYYY-MM-DD HH24:MI:SS.FF6"},
	               [bytes](DirectPathLoader &l, ub4 r, int64_t i) {
		               char *p = reinterpret_cast<char *>(bytes->data() + r * 32);
		               FormatDateTimeText(DayValue(i), MicrosOfDay(i), p);
		               l.SetCell(r, 0, p, 26);
	               },
	               ts_check, ts_expected, {}});
	out.push_back({"TIMESTAMP(6), internal type 180 (7/11 bytes)", "PROBE_TS", "C TIMESTAMP(6)",
	               {"C", kInternalTimestamp, 11, ""},
	               [bytes](DirectPathLoader &l, ub4 r, int64_t i) {
		               uint8_t *p = bytes->data() + r * 32;
		               l.SetCell(r, 0, p, static_cast<ub4>(EncodeOracleTimestamp(DayValue(i), MicrosOfDay(i), p)));
	               },
	               ts_check, ts_expected, {}});

	// BINARY_DOUBLE: exact sum in half units
	const int64_t halves = kRows * (kRows - 1) / 2 - 2000 * kRows;
	const std::string bd_check = "SELECT COUNT(*) || '/' || TO_CHAR(SUM(C), 'FM999999999999990.0') FROM PROBE_BD";
	const std::string bd_expected = CountSlash(std::to_string(halves / 2) + (halves % 2 ? ".5" : ".0"));
	out.push_back({"BINARY_DOUBLE, SQLT_BDOUBLE (native)", "PROBE_BD", "C BINARY_DOUBLE", {"C", SQLT_BDOUBLE, 8, ""},
	               [doubles](DirectPathLoader &l, ub4 r, int64_t i) {
		               (*doubles)[r] = DoubleValue(i);
		               l.SetCell(r, 0, &(*doubles)[r], 8);
	               },
	               bd_check, bd_expected, {}});
	out.push_back({"BINARY_DOUBLE, SQLT_FLT (native)", "PROBE_BD", "C BINARY_DOUBLE", {"C", SQLT_FLT, 8, ""},
	               [doubles](DirectPathLoader &l, ub4 r, int64_t i) {
		               (*doubles)[r] = DoubleValue(i);
		               l.SetCell(r, 0, &(*doubles)[r], 8);
	               },
	               bd_check, bd_expected, {}});

	// VARCHAR2
	int64_t len_sum = 0;
	for (int64_t i = 0; i < kRows; i++) {
		len_sum += 4 + static_cast<int64_t>(std::to_string(i * 13).size());
	}
	out.push_back({"VARCHAR2(30), SQLT_CHR", "PROBE_VC", "C VARCHAR2(30)", {"C", SQLT_CHR, 30, ""},
	               [texts](DirectPathLoader &l, ub4 r, int64_t i) {
		               (*texts)[r] = "val_" + std::to_string(i * 13);
		               l.SetCell(r, 0, (*texts)[r].data(), static_cast<ub4>((*texts)[r].size()));
	               },
	               "SELECT COUNT(*) || '/' || SUM(LENGTH(C)) FROM PROBE_VC", CountSlash(std::to_string(len_sum)), {}});
	return out;
}

// Realistic 5-column table, using the extension's default representations
const char *kMixDdl = "ID NUMBER(19), MONTANT NUMBER(15,2), LIBELLE VARCHAR2(30), D DATE, TS TIMESTAMP(6)";

std::vector<DirPathColumnSpec> MixSpecs() {
	return {{"ID", SQLT_INT, 8, ""},
	        {"MONTANT", SQLT_NUM, 22, ""},
	        {"LIBELLE", SQLT_CHR, 30, ""},
	        {"D", SQLT_DAT, 7, ""},
	        {"TS", SQLT_CHR, 26, "YYYY-MM-DD HH24:MI:SS.FF6"}};
}

struct MixRows {
	std::vector<int64_t> ids = std::vector<int64_t>(kMaxArrayRows);
	std::vector<uint8_t> num = std::vector<uint8_t>(kMaxArrayRows * kOracleNumberMaxBytes);
	std::vector<std::string> text = std::vector<std::string>(kMaxArrayRows);
	std::vector<uint8_t> dat = std::vector<uint8_t>(kMaxArrayRows * kOracleDateBytes);
	std::vector<char> ts = std::vector<char>(kMaxArrayRows * kDateTimeTextBytes);

	void Write(DirectPathLoader &l, ub4 r, int64_t i) {
		ids[r] = i;
		l.SetCell(r, 0, &ids[r], 8);
		const int64_t cents = CentsValue(i);
		uint8_t *n = num.data() + r * kOracleNumberMaxBytes;
		l.SetCell(r, 1, n, static_cast<ub4>(EncodeOracleNumber(Magnitude(cents), cents < 0, 2, n)));
		text[r] = "libelle_" + std::to_string(i % 100000);
		l.SetCell(r, 2, text[r].data(), static_cast<ub4>(text[r].size()));
		uint8_t *d = dat.data() + r * kOracleDateBytes;
		EncodeOracleDate(DayValue(i), SecondOfDay(i) * kMicrosPerSecond, d);
		l.SetCell(r, 3, d, kOracleDateBytes);
		char *t = ts.data() + r * kDateTimeTextBytes;
		FormatDateTimeText(DayValue(i), MicrosOfDay(i), t);
		l.SetCell(r, 4, t, kDateTimeTextBytes);
	}
};

void MixRun(OciSession &s, const std::string &label, const LoaderOptions &options) {
	try {
		Recreate(s, "PROBE_MIX", kMixDdl);
		ub4 rows = 0;
		bool contiguous = false;
		const double t = Seconds([&] {
			DirectPathLoader loader(Credentials(), "BENCH", "PROBE_MIX", MixSpecs(), options);
			rows = loader.Rows();
			contiguous = loader.contiguous_arrays();
			MixRows w;
			Feed(loader, 0, kRows, [&](DirectPathLoader &l, ub4 r, int64_t i) { w.Write(l, r, i); });
			loader.Finish();
		});
		const std::string count = Scalar(s, "SELECT COUNT(*) FROM PROBE_MIX");
		std::printf("| %s | %.3f | %.0f | rows=%u contigu=%s | %s |\n", label.c_str(), t, kRows / t, rows,
		            contiguous ? "yes" : "no", count == std::to_string(kRows) ? "OK" : ("FAILED " + count).c_str());
	} catch (const std::exception &e) {
		std::printf("| %s | — | — | — | ERROR: %s |\n", label.c_str(), e.what());
	}
	std::fflush(stdout);
}

void ParallelRun(OciSession &s, int sessions, int64_t total) {
	Recreate(s, "PROBE_MIX", kMixDdl);
	std::vector<std::unique_ptr<DirectPathLoader>> loaders(sessions);
	std::vector<std::string> errors(sessions);
	std::vector<double> finish(sessions, 0.0);
	const double t = Seconds([&] {
		std::vector<std::thread> threads;
		for (int k = 0; k < sessions; k++) {
			threads.emplace_back([&, k] {
				try {
					loaders[k] = std::make_unique<DirectPathLoader>(Credentials(), "BENCH", "PROBE_MIX", MixSpecs(),
					                                                LoaderOptions {});
					MixRows w;
					const int64_t per = total / sessions;
					Feed(*loaders[k], k * per, per, [&](DirectPathLoader &l, ub4 r, int64_t i) { w.Write(l, r, i); });
				} catch (const std::exception &e) {
					errors[k] = e.what();
				}
			});
		}
		for (auto &th : threads) {
			th.join();
		}
		std::vector<std::thread> finishers;
		for (int k = 0; k < sessions; k++) {
			finishers.emplace_back([&, k] {
				if (!loaders[k]) {
					return;
				}
				try {
					finish[k] = Seconds([&] { loaders[k]->Finish(); });
				} catch (const std::exception &e) {
					errors[k] = e.what();
				}
			});
		}
		for (auto &th : finishers) {
			th.join();
		}
	});
	std::string error;
	for (auto &e : errors) {
		if (!e.empty()) {
			error = e;
		}
	}
	const std::string count = Scalar(s, "SELECT COUNT(*) FROM PROBE_MIX");
	std::printf("| %d | %.3f | %.0f | %.3f | %s |\n", sessions, t, total / t,
	            *std::max_element(finish.begin(), finish.end()),
	            error.empty() ? (count == std::to_string(total) ? "OK" : ("FAILED " + count).c_str())
	                          : ("ERROR: " + error).c_str());
	std::fflush(stdout);
}

void MixedCase(OciSession &s) {
	auto attempt = [&](const std::string &label, const std::string &table, const std::string &column) {
		try {
			Recreate(s, "\"ProbeMixed\"", "\"MaCol\" NUMBER");
			DirectPathLoader loader(Credentials(), "BENCH", table, {{column, SQLT_INT, 8, ""}}, LoaderOptions {});
			const int64_t v = 7;
			loader.SetCell(0, 0, &v, 8);
			loader.ConvertAndLoad(1);
			loader.Finish();
			std::printf("| %s | OK (%s row) |\n", label.c_str(),
			            Scalar(s, "SELECT COUNT(*) FROM \"ProbeMixed\"").c_str());
		} catch (const std::exception &e) {
			std::printf("| %s | ERROR: %s |\n", label.c_str(), e.what());
		}
	};
	attempt("raw names: ProbeMixed / MaCol", "ProbeMixed", "MaCol");
	attempt("quoted names: \"ProbeMixed\" / \"MaCol\"", "\"ProbeMixed\"", "\"MaCol\"");
	Drop(s, "\"ProbeMixed\"");
}

void IndexedTable(OciSession &s) {
	Recreate(s, "PROBE_IDX", "ID NUMBER");
	s.Execute("CREATE INDEX PROBE_IDX_I ON PROBE_IDX (ID)");
	try {
		DirectPathLoader loader(Credentials(), "BENCH", "PROBE_IDX", {{"ID", SQLT_INT, 8, ""}}, LoaderOptions {});
		const int64_t v = 1;
		loader.SetCell(0, 0, &v, 8);
		loader.ConvertAndLoad(1);
		loader.Finish();
		std::printf("No error; rows loaded: %s; index: %s\n", Scalar(s, "SELECT COUNT(*) FROM PROBE_IDX").c_str(),
		            Scalar(s, "SELECT status FROM user_indexes WHERE index_name = 'PROBE_IDX_I'").c_str());
	} catch (const OracleError &e) {
		std::printf("ORA-%05d : %s\n", e.ora_code, e.what());
	}
	Drop(s, "PROBE_IDX");
}

// Cross-check of the encoders against DUMP()
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

std::string Dump(int typ, const uint8_t *bytes, size_t n) {
	std::string out = "Typ=" + std::to_string(typ) + " Len=" + std::to_string(n) + ": ";
	for (size_t i = 0; i < n; i++) {
		out += (i ? "," : "") + std::to_string(bytes[i]);
	}
	return out;
}

void DumpCheck(OciSession &s) {
	Recreate(s, "PROBE_DUMP", "K NUMBER, N NUMBER, D DATE, TS TIMESTAMP(6)");
	const std::vector<std::string> numbers = {"0", "1", "-1", "100", "123", "-123", "0.5", "-0.5", "12.34", "123.45",
	                                          "0.01", "0.001", "1.1", "1000000", "9223372036854775807",
	                                          "-9223372036854775808", "18446744073709551615",
	                                          "99999999999999999999999999999999999999",
	                                          "-9999999999999999999999999999.9999999999"};
	for (size_t k = 0; k < numbers.size(); k++) {
		s.Execute("INSERT INTO PROBE_DUMP (K, N) VALUES (" + std::to_string(k) + ", " + numbers[k] + ")");
	}
	const std::vector<std::string> datetimes = {"1970-01-01 00:00:00.000000", "1969-12-31 23:59:59.999999",
	                                            "2024-02-29 13:45:07.123456", "0001-01-01 00:00:00.000000",
	                                            "9999-12-31 23:59:59.999999"};
	for (size_t k = 0; k < datetimes.size(); k++) {
		s.Execute("INSERT INTO PROBE_DUMP (K, D, TS) VALUES (" + std::to_string(100 + k) + ", TO_DATE('" +
		          datetimes[k].substr(0, 19) + "', 'YYYY-MM-DD HH24:MI:SS'), TO_TIMESTAMP('" + datetimes[k] +
		          "', 'YYYY-MM-DD HH24:MI:SS.FF6'))");
	}
	int ok = 0, total = 0;
	for (size_t k = 0; k < numbers.size(); k++) {
		const std::string &lit = numbers[k];
		const bool negative = lit[0] == '-';
		unsigned __int128 magnitude = 0; // the probe is built with GCC only
		uint8_t scale = 0;
		bool frac = false;
		for (char c : lit) {
			if (c == '.') {
				frac = true;
			} else if (c != '-') {
				magnitude = magnitude * 10 + static_cast<unsigned>(c - '0');
				scale += frac ? 1 : 0;
			}
		}
		uint8_t buf[kOracleNumberMaxBytes];
		const OracleUint128 parts {static_cast<uint64_t>(magnitude >> 64), static_cast<uint64_t>(magnitude)};
		const std::string ours = Dump(2, buf, EncodeOracleNumber(parts, negative, scale, buf));
		const std::string oracle = Scalar(s, "SELECT DUMP(N) FROM PROBE_DUMP WHERE K = " + std::to_string(k));
		total++;
		if (ours == oracle) {
			ok++;
		} else {
			std::printf("- NUMBER %s : OraDuck `%s` / Oracle `%s`\n", lit.c_str(), ours.c_str(), oracle.c_str());
		}
	}
	for (size_t k = 0; k < datetimes.size(); k++) {
		const std::string &lit = datetimes[k];
		const int64_t days = DaysFromCivil(std::stoll(lit.substr(0, 4)), std::stoul(lit.substr(5, 2)),
		                                   std::stoul(lit.substr(8, 2)));
		const int64_t micros = (std::stoll(lit.substr(11, 2)) * 3600 + std::stoll(lit.substr(14, 2)) * 60 +
		                        std::stoll(lit.substr(17, 2))) * kMicrosPerSecond + std::stoll(lit.substr(20, 6));
		uint8_t dat[kOracleDateBytes];
		uint8_t ts[kOracleTimestampBytes];
		EncodeOracleDate(days, micros, dat);
		const size_t ts_len = EncodeOracleTimestamp(days, micros, ts); // 7 bytes without fraction
		const std::string ours_d = Dump(12, dat, kOracleDateBytes);
		const std::string ours_ts = Dump(180, ts, ts_len);
		const std::string oracle_d = Scalar(s, "SELECT DUMP(D) FROM PROBE_DUMP WHERE K = " + std::to_string(100 + k));
		const std::string oracle_ts = Scalar(s, "SELECT DUMP(TS) FROM PROBE_DUMP WHERE K = " + std::to_string(100 + k));
		total += 2;
		ok += (ours_d == oracle_d) + (ours_ts == oracle_ts);
		if (ours_d != oracle_d) {
			std::printf("- DATE %s : OraDuck `%s` / Oracle `%s`\n", lit.c_str(), ours_d.c_str(), oracle_d.c_str());
		}
		if (ours_ts != oracle_ts) {
			std::printf("- TIMESTAMP %s : OraDuck `%s` / Oracle `%s`\n", lit.c_str(), ours_ts.c_str(),
			            oracle_ts.c_str());
		}
	}
	std::printf("\nEncoders matching DUMP(): %d / %d\n", ok, total);
	Drop(s, "PROBE_DUMP");
}

} // namespace

int main() {
	OciSession s(Credentials());
	std::printf("# OraDuck probe results\n\nOracle %s, %lld rows per variant, client pinned by the caller\n",
	            Scalar(s, "SELECT version_full FROM product_component_version WHERE ROWNUM = 1").c_str(),
	            static_cast<long long>(kRows));

	std::printf("\n## E9 — Encoders and DUMP()\n\n");
	DumpCheck(s);

	std::printf("\n## E1/E2 — Single-column representations (1 session)\n\n| Variant | Time (s) | Rows/s | Check |\n|---|---|---|---|\n");
	for (const auto &variant : SingleColumnVariants()) {
		RunVariant(s, variant);
	}

	const char *mix_header = "| Setting | Time (s) | Rows/s | Column array | Check |\n|---|---|---|---|---|\n";
	std::printf("\n## E3 — Stream accumulation (5 columns)\n\n%s", mix_header);
	MixRun(s, "accumulate_stream = false", LoaderOptions {});
	LoaderOptions accumulate;
	accumulate.accumulate_stream = true;
	MixRun(s, "accumulate_stream = true", accumulate);

	std::printf("\n## E4 — Stream buffer size (5 columns)\n\n%s", mix_header);
	for (ub4 size : {64u << 10, 256u << 10, 1u << 20, 4u << 20, 16u << 20}) {
		LoaderOptions options;
		options.stream_buffer_bytes = size;
		MixRun(s, "stream " + std::to_string(size / 1024) + " KiB", options);
	}

	std::printf("\n## E5 — Rows per column array (5 columns)\n\n%s", mix_header);
	for (ub4 rows : {2048u, 8192u}) {
		LoaderOptions options;
		options.column_array_rows = rows;
		MixRun(s, "column_array_rows = " + std::to_string(rows), options);
	}

	std::printf("\n## E6 — Parallel sessions (2,000,000 rows, 5 columns)\n\n| Sessions | Time (s) | Rows/s | Max Finish (s) | Check |\n|---|---|---|---|---|\n");
	for (int sessions : {1, 2, 4, 8}) {
		ParallelRun(s, sessions, 2000000);
	}

	std::printf("\n## E7 — Mixed-case identifiers\n\n| Attempt | Result |\n|---|---|\n");
	MixedCase(s);

	std::printf("\n## E8 — Indexed table (PARALLEL = TRUE)\n\n");
	IndexedTable(s);

	for (const char *table : {"PROBE_NUM", "PROBE_DEC", "PROBE_DATE", "PROBE_TS", "PROBE_BD", "PROBE_VC", "PROBE_MIX"}) {
		Drop(s, table);
	}
	return 0;
}

// OraDuck read probe: OCI fetch techniques for oraduck_scan, measured on Oracle 19c.
// Usage: source env.sh && taskset -c 0-7 make read-probe | tee build/read-probe.txt
// Arguments (READ_PROBE_ARGS with make): section names (R1 ... R7) run only those sections;
// --reuse keeps the probe tables at the end and reuses those that already have the expected rows.
#include "oraduck/oci_session.hpp"
#include "oraduck/oracle_decoding.hpp"
#include "oraduck/oracle_encoding.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h> // getpid (the probe is Linux-only, like ss)

using namespace oraduck;

namespace {

typedef __int128 Int128; // the probe is built with GCC only

bool g_reuse = false;
std::set<std::string> g_tables; // probe tables, dropped at the end unless --reuse

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

// --- Formatting and timing ---

std::string Thousands(double value) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%.0f", value);
	const std::string digits(buffer);
	const size_t start = digits[0] == '-' ? 1 : 0;
	std::string out = digits.substr(0, start);
	for (size_t i = start; i < digits.size(); i++) {
		if (i > start && (digits.size() - i) % 3 == 0) {
			out += ',';
		}
		out += digits[i];
	}
	return out;
}

std::string Fixed(double value, int decimals) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
	return buffer;
}

std::string ToString(Int128 value) {
	if (value == 0) {
		return "0";
	}
	const bool negative = value < 0;
	unsigned __int128 magnitude = negative ? -static_cast<unsigned __int128>(value) : value;
	std::string out;
	while (magnitude) {
		out += static_cast<char>('0' + static_cast<int>(magnitude % 10));
		magnitude /= 10;
	}
	if (negative) {
		out += '-';
	}
	return std::string(out.rbegin(), out.rend());
}

double Since(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

struct Timing {
	double wall;
	double cpu; // process CPU time (every thread, OCI included)
};

Timing Measure(const std::function<void()> &fn) {
	const auto start = std::chrono::steady_clock::now();
	const std::clock_t cpu = std::clock();
	fn();
	return {Since(start), static_cast<double>(std::clock() - cpu) / CLOCKS_PER_SEC};
}

void Section(const std::string &title) {
	std::printf("\n## %s\n\n", title.c_str());
	std::fflush(stdout);
	if (FILE *load = std::fopen("/proc/loadavg", "r")) { // machine load, for the record (stderr)
		char text[128] = {0};
		if (std::fgets(text, sizeof(text), load)) {
			std::fprintf(stderr, "%s: load average %s", title.substr(0, 2).c_str(), text);
		}
		std::fclose(load);
	}
}

// --- Tables ---

std::string Scalar(OciSession &s, const std::string &sql) {
	return s.QueryStrings(sql).at(0).at(0);
}

void Drop(OciSession &s, const std::string &table) {
	s.Execute("BEGIN EXECUTE IMMEDIATE 'DROP TABLE " + table +
	          " PURGE'; EXCEPTION WHEN OTHERS THEN IF SQLCODE != -942 THEN RAISE; END IF; END;");
}

// Rows 1..rows (a multiple of 1,000) as column R: two CONNECT BY generators joined
std::string Rows(int64_t rows) {
	return "(SELECT (a.n - 1) * 1000 + b.n AS r FROM (SELECT LEVEL AS n FROM dual CONNECT BY LEVEL <= " +
	       std::to_string(rows / 1000) + ") a CROSS JOIN (SELECT LEVEL AS n FROM dual CONNECT BY LEVEL <= 1000) b)";
}

// Creates and fills a table (server side), unless --reuse finds it with the expected row count
void Prepare(OciSession &s, const std::string &table, const std::string &ddl, const std::vector<std::string> &fill,
             int64_t rows) {
	if (!g_tables.insert(table).second) {
		return; // already prepared by an earlier section
	}
	if (g_reuse) {
		try {
			if (Scalar(s, "SELECT COUNT(*) FROM " + table) == std::to_string(rows)) {
				return;
			}
		} catch (const OracleError &) {
		}
	}
	Drop(s, table);
	const Timing t = Measure([&] {
		s.Execute(ddl);
		for (const auto &statement : fill) {
			s.Execute(statement);
		}
	});
	std::fprintf(stderr, "%s: %lld rows built in %.1f s\n", table.c_str(), static_cast<long long>(rows), t.wall);
}

// --- Session statistics (V$MYSTAT) ---

struct Stats {
	int64_t round_trips = 0;
	int64_t bytes_sent = 0; // SQL*Net bytes sent to the client
	int64_t consistent_gets = 0;
	int64_t server_cpu = 0; // CPU used by this session, centiseconds
	int64_t requests = 0;   // TCP data segments sent by this process to Oracle (-1: unknown)
};

Stats operator-(const Stats &a, const Stats &b) {
	return {a.round_trips - b.round_trips, a.bytes_sent - b.bytes_sent, a.consistent_gets - b.consistent_gets,
	        a.server_cpu - b.server_cpu, a.requests < 0 || b.requests < 0 ? -1 : a.requests - b.requests};
}

// Client-side request count: data segments sent on this process's TCP connections to the Oracle port, from
// the kernel (ss -ti). Each OCI call is one small request; V$MYSTAT misses some LOB calls (see R4).
int64_t ClientRequests() {
	static const std::string command = [] {
		const char *dsn = std::getenv("ORADUCK_ORACLE_DSN");
		std::string port = "1521";
		if (dsn) {
			const std::string text(dsn);
			const size_t colon = text.rfind(':', text.find('/'));
			if (colon != std::string::npos) {
				port = text.substr(colon + 1, text.find('/', colon) - colon - 1);
			}
		}
		return "ss -tinpH dst :" + port + " 2>/dev/null";
	}();
	FILE *pipe = popen(command.c_str(), "r");
	if (!pipe) {
		return -1;
	}
	const std::string pid = "pid=" + std::to_string(getpid()) + ",";
	char line[8192];
	int64_t total = 0;
	bool mine = false, seen = false;
	while (std::fgets(line, sizeof(line), pipe)) {
		const std::string text(line);
		if (text.find("ESTAB") != std::string::npos) {
			mine = text.find(pid) != std::string::npos;
		}
		const size_t at = text.find("data_segs_out:");
		if (mine && at != std::string::npos) {
			total += std::atoll(text.c_str() + at + 14);
			seen = true;
		}
	}
	pclose(pipe);
	return seen ? total : -1;
}

class StatReader {
public:
	explicit StatReader(OciSession &session) : session_(session) {
		const Stats first = Read();
		overhead_ = Read() - first;
	}

	Stats Read() {
		Stats out;
		out.requests = ClientRequests();
		for (const auto &row : session_.QueryStrings(
		         "SELECT n.name, s.value FROM v$mystat s JOIN v$statname n ON n.statistic# = s.statistic# "
		         "WHERE n.name IN ('SQL*Net roundtrips to/from client', 'bytes sent via SQL*Net to client', "
		         "'consistent gets', 'CPU used by this session')")) {
			const int64_t value = std::stoll(row[1]);
			if (row[0] == "SQL*Net roundtrips to/from client") {
				out.round_trips = value;
			} else if (row[0] == "bytes sent via SQL*Net to client") {
				out.bytes_sent = value;
			} else if (row[0] == "consistent gets") {
				out.consistent_gets = value;
			} else {
				out.server_cpu = value;
			}
		}
		return out;
	}

	// Activity since before, without the cost of the two readings
	Stats Since(const Stats &before) {
		return Read() - before - overhead_;
	}

	const Stats &overhead() const {
		return overhead_;
	}

private:
	OciSession &session_;
	Stats overhead_;
};

// --- Array fetch cursor ---

struct ColumnSpec {
	ub2 dty;
	sb8 width = 0;        // bytes per value in the define buffer
	ub4 descriptor = 0;   // OCI_DTYPE_*: one descriptor per row instead of inline bytes
	ub4 lob_prefetch = 0; // OCI_ATTR_LOBPREFETCH_SIZE of a locator column (characters for a CLOB)
	bool dynamic = false; // OCI_DYNAMIC_FETCH: pieces through a callback
};

struct FetchSettings {
	ub4 rows = 2048;
	ub4 prefetch_rows = 0;
	ub4 prefetch_memory = 0;
};

constexpr ub4 kChunkBytes = 64 * 1024;

struct Chunk {
	std::unique_ptr<uint8_t[]> data;
	ub4 length = 0;
};

// Pieces of the values of one dynamically defined column (OCI_DYNAMIC_FETCH)
struct DynamicBuffer {
	explicit DynamicBuffer(ub4 rows) : pieces(rows), ind(rows, -1), rcode(rows, 0) {
	}
	std::vector<std::unique_ptr<Chunk>> pool; // stable addresses: OCI keeps pointers to the lengths
	size_t used = 0;
	std::vector<std::vector<Chunk *>> pieces; // per row
	std::vector<sb2> ind;
	std::vector<ub2> rcode;

	void Reset() {
		used = 0;
		for (auto &row : pieces) {
			row.clear();
		}
		std::fill(ind.begin(), ind.end(), -1);
	}
};

sb4 DynamicCallback(void *octxp, OCIDefine *, ub4 iter, void **bufpp, ub4 **alenp, ub1 *, void **indp, ub2 **rcodep) {
	auto &buffer = *static_cast<DynamicBuffer *>(octxp);
	if (buffer.used == buffer.pool.size()) {
		buffer.pool.push_back(std::unique_ptr<Chunk>(new Chunk()));
		buffer.pool.back()->data.reset(new uint8_t[kChunkBytes]);
	}
	Chunk *chunk = buffer.pool[buffer.used++].get();
	chunk->length = kChunkBytes;
	buffer.pieces[iter].push_back(chunk);
	*bufpp = chunk->data.get();
	*alenp = &chunk->length;
	*indp = &buffer.ind[iter];
	*rcodep = &buffer.rcode[iter];
	return OCI_CONTINUE;
}

const OraText *Text(const std::string &value) {
	return reinterpret_cast<const OraText *>(value.data());
}

const uint8_t kEmpty[1] = {0};

class Cursor {
public:
	// Prepares, binds (text values for :1, :2...), defines and executes the query
	Cursor(OciSession &session, const std::string &sql, const std::vector<std::string> &binds,
	       const std::vector<ColumnSpec> &columns, const FetchSettings &settings)
	    : session_(session), rows_(settings.rows) {
		try {
			Open(sql, binds, columns, settings);
		} catch (...) {
			Close();
			throw;
		}
	}
	~Cursor() {
		Close();
	}
	Cursor(const Cursor &) = delete;
	Cursor &operator=(const Cursor &) = delete;

	// Rows of the next batch, 0 at the end
	ub4 Fetch() {
		if (done_) {
			return 0;
		}
		for (auto &column : columns_) {
			if (column.dynamic) {
				column.dynamic->Reset();
			}
		}
		OCIError *err = session_.err();
		const sword rc = Oci().OCIStmtFetch2(stmt_, err, rows_, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
		if (rc == OCI_NO_DATA) {
			done_ = true;
		} else {
			CheckOci(rc, err, "OCIStmtFetch2");
			if (rc == OCI_SUCCESS_WITH_INFO && info_.empty()) {
				int32_t code = 0;
				info_ = OciErrorText(err, rc, code);
			}
		}
		ub4 fetched = 0;
		CheckOci(Oci().OCIAttrGet(stmt_, OCI_HTYPE_STMT, &fetched, nullptr, OCI_ATTR_ROWS_FETCHED, err), err,
		         "OCI_ATTR_ROWS_FETCHED");
		return fetched;
	}

	bool Null(size_t c, ub4 r) const {
		const Column &column = columns_[c];
		return (column.dynamic ? column.dynamic->ind[r] : column.ind[r]) == -1;
	}
	const uint8_t *Bytes(size_t c, ub4 r) const {
		return columns_[c].data.data() + static_cast<size_t>(r) * columns_[c].spec.width;
	}
	ub4 Length(size_t c, ub4 r) const {
		return columns_[c].len[r];
	}
	int64_t Int(size_t c, ub4 r) const {
		int64_t value;
		std::memcpy(&value, Bytes(c, r), sizeof(value));
		return value;
	}
	void *Descriptor(size_t c, ub4 r) const {
		return columns_[c].descriptors[r];
	}
	// Value of a dynamic column: its single piece in place, or its pieces joined into scratch
	const uint8_t *Pieces(size_t c, ub4 r, size_t &length, std::string &scratch) const {
		const auto &pieces = columns_[c].dynamic->pieces[r];
		if (pieces.empty()) {
			length = 0;
			return kEmpty;
		}
		if (pieces.size() == 1) {
			length = pieces[0]->length;
			return pieces[0]->data.get();
		}
		scratch.clear();
		for (const Chunk *piece : pieces) {
			scratch.append(reinterpret_cast<const char *>(piece->data.get()), piece->length);
		}
		length = scratch.size();
		return reinterpret_cast<const uint8_t *>(scratch.data());
	}
	// Text of the first OCI_SUCCESS_WITH_INFO of the fetches (empty if none)
	const std::string &info() const {
		return info_;
	}

private:
	struct Column {
		ColumnSpec spec;
		std::vector<uint8_t> data;
		std::vector<void *> descriptors;
		std::vector<sb2> ind;
		std::vector<ub4> len;
		std::vector<ub2> rcode;
		std::unique_ptr<DynamicBuffer> dynamic;
		OCIDefine *define = nullptr;
	};

	void Open(const std::string &sql, const std::vector<std::string> &binds, const std::vector<ColumnSpec> &columns,
	          const FetchSettings &settings) {
		OCIError *err = session_.err();
		CheckOci(Oci().OCIStmtPrepare2(session_.svc(), &stmt_, err, Text(sql), static_cast<ub4>(sql.size()), nullptr, 0,
		                               OCI_NTV_SYNTAX, OCI_DEFAULT),
		         err, "OCIStmtPrepare2");
		for (size_t i = 0; i < binds.size(); i++) {
			OCIBind *bind = nullptr;
			CheckOci(Oci().OCIBindByPos(stmt_, &bind, err, static_cast<ub4>(i + 1), const_cast<char *>(binds[i].data()),
			                            static_cast<sb4>(binds[i].size()), SQLT_CHR, nullptr, nullptr, nullptr, 0,
			                            nullptr, OCI_DEFAULT),
			         err, "OCIBindByPos");
		}
		ub4 prefetch_rows = settings.prefetch_rows;
		ub4 prefetch_memory = settings.prefetch_memory;
		CheckOci(Oci().OCIAttrSet(stmt_, OCI_HTYPE_STMT, &prefetch_rows, 0, OCI_ATTR_PREFETCH_ROWS, err), err,
		         "OCI_ATTR_PREFETCH_ROWS");
		CheckOci(Oci().OCIAttrSet(stmt_, OCI_HTYPE_STMT, &prefetch_memory, 0, OCI_ATTR_PREFETCH_MEMORY, err), err,
		         "OCI_ATTR_PREFETCH_MEMORY");
		columns_.resize(columns.size());
		for (size_t i = 0; i < columns.size(); i++) {
			Define(static_cast<ub4>(i + 1), columns[i], columns_[i]);
		}
		CheckOci(Oci().OCIStmtExecute(session_.svc(), stmt_, err, 0, 0, nullptr, nullptr, OCI_DEFAULT), err,
		         "OCIStmtExecute");
	}

	void Define(ub4 position, const ColumnSpec &spec, Column &column) {
		OCIError *err = session_.err();
		column.spec = spec;
		if (spec.dynamic) {
			column.dynamic.reset(new DynamicBuffer(rows_));
			CheckOci(Oci().OCIDefineByPos2(stmt_, &column.define, err, position, nullptr, 0x7FFFFFFF, spec.dty, nullptr,
			                               nullptr, nullptr, OCI_DYNAMIC_FETCH),
			         err, "OCIDefineByPos2 (OCI_DYNAMIC_FETCH)");
			CheckOci(Oci().OCIDefineDynamic(column.define, err, column.dynamic.get(), DynamicCallback), err,
			         "OCIDefineDynamic");
			return;
		}
		column.ind.resize(rows_);
		column.len.resize(rows_);
		column.rcode.resize(rows_);
		void *values = nullptr;
		sb8 width = spec.width;
		if (spec.descriptor) {
			column.descriptors.resize(rows_, nullptr);
			for (auto &descriptor : column.descriptors) {
				CheckOci(Oci().OCIDescriptorAlloc(session_.env(), &descriptor, spec.descriptor, 0, nullptr), nullptr,
				         "OCIDescriptorAlloc");
			}
			values = column.descriptors.data();
			width = sizeof(void *);
		} else {
			column.data.resize(static_cast<size_t>(rows_) * spec.width);
			values = column.data.data();
		}
		CheckOci(Oci().OCIDefineByPos2(stmt_, &column.define, err, position, values, width, spec.dty, column.ind.data(),
		                               column.len.data(), column.rcode.data(), OCI_DEFAULT),
		         err, "OCIDefineByPos2");
		if (spec.lob_prefetch) {
			ub4 size = spec.lob_prefetch;
			int length = 1;
			CheckOci(Oci().OCIAttrSet(column.define, OCI_HTYPE_DEFINE, &size, 0, OCI_ATTR_LOBPREFETCH_SIZE, err), err,
			         "OCI_ATTR_LOBPREFETCH_SIZE");
			CheckOci(Oci().OCIAttrSet(column.define, OCI_HTYPE_DEFINE, &length, 0, OCI_ATTR_LOBPREFETCH_LENGTH, err),
			         err, "OCI_ATTR_LOBPREFETCH_LENGTH");
		}
	}

	void Close() {
		if (stmt_) {
			Oci().OCIStmtRelease(stmt_, session_.err(), nullptr, 0, OCI_DEFAULT);
			stmt_ = nullptr;
		}
		for (auto &column : columns_) {
			for (void *descriptor : column.descriptors) {
				if (descriptor) {
					Oci().OCIDescriptorFree(descriptor, column.spec.descriptor);
				}
			}
		}
		columns_.clear();
	}

	OciSession &session_;
	ub4 rows_;
	OCIStmt *stmt_ = nullptr;
	std::vector<Column> columns_;
	bool done_ = false;
	std::string info_;
};

// --- Single-session scans ---

struct Run {
	double wall = 0;
	double cpu = 0;
	double decode = 0; // seconds spent decoding values (inside the sink)
	int64_t rows = 0;
	int64_t values = 0; // non-NULL values decoded
	Stats stats;
	std::string info;
};

using Sink = std::function<void(Cursor &, ub4 rows)>;

Run Scan(OciSession &s, StatReader &stat, const std::string &sql, const std::vector<std::string> &binds,
         const std::vector<ColumnSpec> &columns, const FetchSettings &settings, const Sink &sink) {
	Run run;
	const Stats before = stat.Read();
	const Timing t = Measure([&] {
		Cursor cursor(s, sql, binds, columns, settings);
		for (ub4 n; (n = cursor.Fetch()) > 0;) {
			sink(cursor, n);
			run.rows += n;
		}
		run.info = cursor.info();
	});
	run.stats = stat.Since(before);
	run.wall = t.wall;
	run.cpu = t.cpu;
	return run;
}

// Median by wall time
Run Median(std::vector<Run> runs) {
	std::sort(runs.begin(), runs.end(), [](const Run &a, const Run &b) { return a.wall < b.wall; });
	return runs[runs.size() / 2];
}

double PerTenThousand(const Run &run) {
	return run.rows ? 10000.0 * static_cast<double>(run.stats.round_trips) / static_cast<double>(run.rows) : 0;
}

std::string NsPerValue(const Run &run) {
	return run.values ? Fixed(1e9 * run.decode / static_cast<double>(run.values), 1) : "—";
}

constexpr int kRepetitions = 3;
constexpr int64_t kNarrowRows = 2000000;

// Times the decode of a batch into run.decode
class DecodeClock {
public:
	explicit DecodeClock(double &total) : total_(total), start_(std::chrono::steady_clock::now()) {
	}
	~DecodeClock() {
		total_ += Since(start_);
	}

private:
	double &total_;
	std::chrono::steady_clock::time_point start_;
};

// --- Date and time text ---

// "[ |-]YYYY-MM-DD HH24:MI:SS.FF6" (TO_CHAR with YYYY or SYYYY) -> microseconds since 1970-01-01
bool TextMicros(const char *p, size_t n, int64_t &micros) {
	bool negative = false;
	if (n == 27 && (p[0] == ' ' || p[0] == '-' || p[0] == '+')) {
		negative = p[0] == '-';
		p++;
		n--;
	}
	if (n != 26 || p[4] != '-' || p[7] != '-' || p[10] != ' ' || p[13] != ':' || p[16] != ':' || p[19] != '.') {
		return false;
	}
	bool ok = true;
	auto digits = [&](int at, int count) {
		int64_t value = 0;
		for (int i = at; i < at + count; i++) {
			ok = ok && p[i] >= '0' && p[i] <= '9';
			value = value * 10 + (p[i] - '0');
		}
		return value;
	};
	const int64_t year = digits(0, 4), month = digits(5, 2), day = digits(8, 2), hour = digits(11, 2),
	              minute = digits(14, 2), second = digits(17, 2), fraction = digits(20, 6);
	if (!ok) {
		return false;
	}
	const int64_t oracle_year = negative ? -year : year;
	const int64_t astronomical = oracle_year < 0 ? oracle_year + 1 : oracle_year;
	micros = DaysFromCivil(astronomical, static_cast<uint32_t>(month), static_cast<uint32_t>(day)) * kMicrosPerDay +
	         ((hour * 60 + minute) * 60 + second) * kMicrosPerSecond + fraction;
	return true;
}

bool DescriptorMicros(OciSession &s, void *descriptor, int64_t &micros) {
	auto *value = static_cast<OCIDateTime *>(descriptor);
	sb2 year = 0;
	ub1 month = 0, day = 0, hour = 0, minute = 0, second = 0;
	ub4 nanos = 0;
	if (Oci().OCIDateTimeGetDate(s.env(), s.err(), value, &year, &month, &day) != OCI_SUCCESS ||
	    Oci().OCIDateTimeGetTime(s.env(), s.err(), value, &hour, &minute, &second, &nanos) != OCI_SUCCESS) {
		return false;
	}
	const int64_t astronomical = year < 0 ? year + 1 : year;
	micros = DaysFromCivil(astronomical, month, day) * kMicrosPerDay +
	         ((static_cast<int64_t>(hour) * 60 + minute) * 60 + second) * kMicrosPerSecond + nanos / 1000;
	return true;
}

// Does a define of type 180 work? Raw OCIDefineByPos (ub2 lengths) on a one-row query
std::string Probe180(OciSession &s, const std::string &sql, int &len7, int &len11) {
	OCIError *err = s.err();
	OCIStmt *stmt = nullptr;
	len7 = len11 = 0;
	try {
		CheckOci(Oci().OCIStmtPrepare2(s.svc(), &stmt, err, Text(sql), static_cast<ub4>(sql.size()), nullptr, 0,
		                               OCI_NTV_SYNTAX, OCI_DEFAULT),
		         err, "OCIStmtPrepare2");
		constexpr ub4 kRows = 64;
		uint8_t values[kRows][11];
		sb2 ind[kRows];
		ub2 len[kRows];
		ub2 rcode[kRows];
		OCIDefine *define = nullptr;
		CheckOci(
		    Oci().OCIDefineByPos(stmt, &define, err, 1, values, 11, kInternalTimestamp, ind, len, rcode, OCI_DEFAULT),
		    err, "OCIDefineByPos(180)");
		CheckOci(Oci().OCIStmtExecute(s.svc(), stmt, err, 0, 0, nullptr, nullptr, OCI_DEFAULT), err, "OCIStmtExecute");
		int rows = 0;
		while (true) {
			const sword rc = Oci().OCIStmtFetch2(stmt, err, kRows, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
			if (rc != OCI_NO_DATA) {
				CheckOci(rc, err, "OCIStmtFetch2");
			}
			ub4 fetched = 0;
			CheckOci(Oci().OCIAttrGet(stmt, OCI_HTYPE_STMT, &fetched, nullptr, OCI_ATTR_ROWS_FETCHED, err), err,
			         "OCI_ATTR_ROWS_FETCHED");
			for (ub4 r = 0; r < fetched; r++) {
				if (ind[r] != -1) {
					len7 += len[r] == 7;
					len11 += len[r] == 11;
				}
			}
			rows += static_cast<int>(fetched);
			if (rc == OCI_NO_DATA) {
				break;
			}
		}
		Oci().OCIStmtRelease(stmt, err, nullptr, 0, OCI_DEFAULT);
		return "accepted (" + std::to_string(rows) + " rows fetched)";
	} catch (const std::exception &e) {
		if (stmt) {
			Oci().OCIStmtRelease(stmt, err, nullptr, 0, OCI_DEFAULT);
		}
		return std::string("refused: ") + e.what();
	}
}

// Define of TIMESTAMP columns in R3/R5: type 180 when OCI accepts it
ColumnSpec TimestampColumn(OciSession &s) {
	static int accepted = -1;
	if (accepted < 0) {
		int len7 = 0, len11 = 0;
		accepted =
		    Probe180(s, "SELECT CAST(SYSTIMESTAMP AS TIMESTAMP(6)) FROM dual", len7, len11).rfind("accepted", 0) == 0;
	}
	return accepted ? ColumnSpec {kInternalTimestamp, 11} : ColumnSpec {SQLT_CHR, 64};
}

// --- R1 NUMBER(18,0) ---

void R1(OciSession &s, StatReader &stat) {
	Section("R1 — NUMBER(18,0) into BIGINT (2,000,000 rows)");
	Prepare(s, "RP_NUM", "CREATE TABLE RP_NUM (C NUMBER(18,0))",
	        {"INSERT /*+ APPEND */ INTO RP_NUM SELECT CASE WHEN MOD(r, 2) = 0 THEN r ELSE "
	         "MOD(r * 7777777777777, 999999999999999989) END * CASE WHEN MOD(r, 3) = 0 THEN -1 ELSE 1 END FROM " +
	         Rows(kNarrowRows)},
	        kNarrowRows);
	const std::string expected = Scalar(s, "SELECT TO_CHAR(SUM(C)) FROM RP_NUM");
	struct Variant {
		std::string label;
		ColumnSpec column;
		bool decode;
	};
	const std::vector<Variant> variants = {
	    {"`SQLT_NUM` (22 bytes) + `DecodeOracleNumberScaled64`", {SQLT_NUM, 22}, true},
	    {"`SQLT_INT` (8 bytes, OCI conversion)", {SQLT_INT, 8}, false}};
	std::vector<std::vector<Run>> runs(variants.size());
	std::vector<std::string> checks(variants.size(), "OK");
	for (int rep = 0; rep < kRepetitions; rep++) {
		for (size_t v = 0; v < variants.size(); v++) {
			Int128 sum = 0;
			int64_t values = 0, bad = 0;
			double decode = 0;
			Run run = Scan(s, stat, "SELECT C FROM RP_NUM", {}, {variants[v].column}, {}, [&](Cursor &c, ub4 n) {
				DecodeClock clock(decode);
				for (ub4 r = 0; r < n; r++) {
					if (c.Null(0, r)) {
						continue;
					}
					int64_t value = 0;
					if (variants[v].decode) {
						if (DecodeOracleNumberScaled64(c.Bytes(0, r), c.Length(0, r), 0, false, value) !=
						    NumberDecodeStatus::kOk) {
							bad++;
						}
					} else {
						value = c.Int(0, r);
					}
					sum += value;
					values++;
				}
			});
			run.decode = decode;
			run.values = values;
			if (ToString(sum) != expected || bad) {
				checks[v] = "FAIL: sum " + ToString(sum) + ", expected " + expected + ", " + std::to_string(bad) +
				            " decode errors";
			}
			runs[v].push_back(run);
		}
	}
	std::printf("2,048 rows per fetch, prefetch off; median of %d runs. Decode = time spent turning the define "
	            "buffer into int64 (per value).\n\n",
	            kRepetitions);
	std::printf("| Define | Wall (s) | Rows/s | Client CPU (s) | Decode (ns/value) | Round trips | Check |\n"
	            "|---|---|---|---|---|---|---|\n");
	for (size_t v = 0; v < variants.size(); v++) {
		const Run m = Median(runs[v]);
		std::printf("| %s | %.3f | %s | %.3f | %s | %s | %s |\n", variants[v].label.c_str(), m.wall,
		            Thousands(m.rows / m.wall).c_str(), m.cpu, NsPerValue(m).c_str(),
		            Thousands(static_cast<double>(m.stats.round_trips)).c_str(), checks[v].c_str());
	}
}

// --- R2 TIMESTAMP(6) ---

void R2(OciSession &s, StatReader &stat) {
	Section("R2 — TIMESTAMP(6) into TIMESTAMP (2,000,000 rows)");
	const std::string base = "TIMESTAMP '1990-01-01 00:00:00' + NUMTODSINTERVAL(MOD(r * 7919, 1000000000)";
	Prepare(s, "RP_TS", "CREATE TABLE RP_TS (C TIMESTAMP(6))",
	        {"INSERT /*+ APPEND */ INTO RP_TS SELECT CASE WHEN MOD(r, 100) = 0 THEN NULL WHEN MOD(r, 7) = 0 THEN " +
	         base + ", 'SECOND') ELSE " + base + " + MOD(r * 104729, 1000000) / 1000000, 'SECOND') END FROM " +
	         Rows(kNarrowRows)},
	        kNarrowRows);
	const std::vector<std::string> edges = {
	    "0001-01-01 00:00:00.000000",  "9999-12-31 23:59:59.999999",  "1969-12-31 23:59:59.999999",
	    "1970-01-01 00:00:00.000000",  "2024-02-29 13:45:07.000001",  "1582-10-04 12:00:00.500000",
	    "-4712-01-01 00:00:00.000000", "-0001-12-31 23:59:59.999999", "2000-01-01 00:00:00.000000"};
	std::string edge_fill = "BEGIN INSERT INTO RP_TS_EDGE VALUES (0, NULL);";
	for (size_t k = 0; k < edges.size(); k++) {
		edge_fill += " INSERT INTO RP_TS_EDGE VALUES (" + std::to_string(k + 1) + ", TO_TIMESTAMP('" + edges[k] +
		             "', 'SYYYY-MM-DD HH24:MI:SS.FF6'));";
	}
	edge_fill += " END;";
	Prepare(s, "RP_TS_EDGE", "CREATE TABLE RP_TS_EDGE (K NUMBER, C TIMESTAMP(6))", {edge_fill},
	        static_cast<int64_t>(edges.size() + 1));

	int len7 = 0, len11 = 0;
	const std::string accepted = Probe180(s, "SELECT C FROM RP_TS_EDGE", len7, len11);
	std::printf("`OCIDefineByPos(…, 180, 11 bytes)` on the edge values (BC, year 1, 9999, NULL…): %s; values of "
	            "7 bytes: %d, of 11 bytes: %d.\n\n",
	            accepted.c_str(), len7, len11);

	const auto expected_row =
	    s.QueryStrings(
	         "SELECT COUNT(C), TO_CHAR(SUM(TRUNC(C) - DATE '1970-01-01')), "
	         "TO_CHAR(SUM(TO_NUMBER(TO_CHAR(C, 'SSSSS')) * 1000000 + TO_NUMBER(TO_CHAR(C, 'FF6')))) FROM RP_TS")
	        .at(0);
	const Int128 expected =
	    static_cast<Int128>(std::stoll(expected_row[1])) * kMicrosPerDay + std::stoll(expected_row[2]);
	const int64_t expected_values = std::stoll(expected_row[0]);

	enum class Kind { kInternal, kDescriptor, kText };
	struct Variant {
		std::string label;
		Kind kind;
		ColumnSpec column;
		std::string select;
	};
	const std::vector<Variant> variants = {
	    {"type 180 (11 bytes) + `DecodeOracleTimestamp`", Kind::kInternal, {kInternalTimestamp, 11}, "C"},
	    {"`SQLT_TIMESTAMP` descriptors + `OCIDateTimeGetDate/Time`",
		 Kind::kDescriptor,
		 {SQLT_TIMESTAMP, 0, OCI_DTYPE_TIMESTAMP},
		 "C"},
	    {"`TO_CHAR(C, 'YYYY-MM-DD HH24:MI:SS.FF6')` (`SQLT_CHR`) + parse",
		 Kind::kText,
		 {SQLT_CHR, 32},
		 "TO_CHAR(C, 'YYYY-MM-DD HH24:MI:SS.FF6')"}};
	auto decode = [&](const Variant &v, Cursor &c, size_t col, ub4 r, int64_t &micros) {
		switch (v.kind) {
		case Kind::kInternal:
			return DecodeOracleTimestamp(c.Bytes(col, r), c.Length(col, r), micros);
		case Kind::kDescriptor:
			return DescriptorMicros(s, c.Descriptor(col, r), micros);
		default:
			return TextMicros(reinterpret_cast<const char *>(c.Bytes(col, r)), c.Length(col, r), micros);
		}
	};

	std::vector<std::vector<Run>> runs(variants.size());
	std::vector<std::string> checks(variants.size(), "OK"), row_checks(variants.size(), "—");
	for (size_t v = 0; v < variants.size(); v++) {
		// Row by row against TO_CHAR: the edge values, then 20,000 rows of the table
		if (variants[v].kind == Kind::kText) {
			continue;
		}
		try {
			int64_t compared = 0, mismatches = 0;
			std::string first;
			Scan(s, stat,
			     "SELECT C, TO_CHAR(C, 'SYYYY-MM-DD HH24:MI:SS.FF6') FROM (SELECT C FROM RP_TS_EDGE UNION ALL "
			     "SELECT C FROM RP_TS WHERE ROWNUM <= 20000)",
			     {}, {variants[v].column, {SQLT_CHR, 32}}, {}, [&](Cursor &c, ub4 n) {
				     for (ub4 r = 0; r < n; r++) {
					     compared++;
					     if (c.Null(0, r) || c.Null(1, r)) {
						     mismatches += c.Null(0, r) != c.Null(1, r);
						     continue;
					     }
					     int64_t got = 0, want = 0;
					     const bool ok_got = decode(variants[v], c, 0, r, got);
					     const bool ok_want =
					         TextMicros(reinterpret_cast<const char *>(c.Bytes(1, r)), c.Length(1, r), want);
					     if (!ok_got || !ok_want || got != want) {
						     if (first.empty()) {
							     first = std::string(reinterpret_cast<const char *>(c.Bytes(1, r)), c.Length(1, r));
						     }
						     mismatches++;
					     }
				     }
			     });
			row_checks[v] = mismatches ? "FAIL: " + std::to_string(mismatches) + " / " + std::to_string(compared) +
			                                 " differ (first: " + first + ")"
			                           : "OK (" + std::to_string(compared) + " rows)";
		} catch (const std::exception &e) {
			row_checks[v] = std::string("ERROR: ") + e.what();
		}
	}
	for (int rep = 0; rep < kRepetitions; rep++) {
		for (size_t v = 0; v < variants.size(); v++) {
			try {
				Int128 sum = 0;
				int64_t values = 0, bad = 0;
				double decode_seconds = 0;
				Run run = Scan(s, stat, "SELECT " + variants[v].select + " FROM RP_TS", {}, {variants[v].column}, {},
				               [&](Cursor &c, ub4 n) {
					               DecodeClock clock(decode_seconds);
					               for (ub4 r = 0; r < n; r++) {
						               if (c.Null(0, r)) {
							               continue;
						               }
						               int64_t micros = 0;
						               if (!decode(variants[v], c, 0, r, micros)) {
							               bad++;
						               }
						               sum += micros;
						               values++;
					               }
				               });
				run.decode = decode_seconds;
				run.values = values;
				if (sum != expected || values != expected_values || bad) {
					checks[v] = "FAIL: sum " + ToString(sum) + ", expected " + ToString(expected) + "; " +
					            std::to_string(bad) + " invalid";
				}
				runs[v].push_back(run);
			} catch (const std::exception &e) {
				checks[v] = std::string("ERROR: ") + e.what();
			}
		}
	}
	std::printf("2,048 rows per fetch, prefetch off; median of %d runs. Sum check: Σ microseconds since 1970 against "
	            "Oracle's `TRUNC`/`SSSSS`/`FF6` sums. Row check: each value against `TO_CHAR(C, "
	            "'SYYYY-MM-DD HH24:MI:SS.FF6')` of the same row.\n\n",
	            kRepetitions);
	std::printf("| Define | Wall (s) | Rows/s | Client CPU (s) | Decode (ns/value) | Round trips | Sum check | Row "
	            "check |\n|---|---|---|---|---|---|---|---|\n");
	for (size_t v = 0; v < variants.size(); v++) {
		if (runs[v].empty()) {
			std::printf("| %s | — | — | — | — | — | %s | %s |\n", variants[v].label.c_str(), checks[v].c_str(),
			            row_checks[v].c_str());
			continue;
		}
		const Run m = Median(runs[v]);
		std::printf("| %s | %.3f | %s | %.3f | %s | %s | %s | %s |\n", variants[v].label.c_str(), m.wall,
		            Thousands(m.rows / m.wall).c_str(), m.cpu, NsPerValue(m).c_str(),
		            Thousands(static_cast<double>(m.stats.round_trips)).c_str(), checks[v].c_str(),
		            row_checks[v].c_str());
	}
}

// --- R3 wide table ---

enum class Slice { kAll, kRowid, kRowidBetween, kHash };
void PreparePartTable(OciSession &s);
std::vector<ColumnSpec> PartColumns(OciSession &s);
std::string PartSql(const std::string &filter, Slice slice, bool as_of);

struct WideGroup {
	char prefix;
	int count;
	const char *type;
	ub2 dty;
	sb8 width;
};

const std::vector<WideGroup> &WideGroups() {
	static const std::vector<WideGroup> groups = {{'V', 200, "VARCHAR2(4000 BYTE)", SQLT_CHR, 4000},
	                                              {'N', 100, "NUMBER(38,0)", SQLT_NUM, 22},
	                                              {'M', 80, "NUMBER(25,9)", SQLT_NUM, 22},
	                                              {'D', 60, "DATE", SQLT_DAT, 7},
	                                              {'C', 50, "CHAR(1 BYTE)", SQLT_CHR, 1},
	                                              {'T', 30, "TIMESTAMP(6)", kInternalTimestamp, 11},
	                                              {'U', 30, "NUMBER", SQLT_NUM, 22}};
	return groups;
}

std::string WideName(char prefix, int j) {
	char name[16];
	std::snprintf(name, sizeof(name), "%c%03d", prefix, j);
	return name;
}

std::string WideExpr(char prefix, int j) {
	const std::string k = std::to_string(j);
	switch (prefix) {
	case 'V': // 5 to 30 characters, 10 % NULL
		return "CASE WHEN MOD(r + " + k + ", 10) = 0 THEN NULL ELSE RPAD(TO_CHAR(r), 5 + MOD(r + " + k +
		       " * 7, 26), 'abcdefghij') END";
	case 'N':
		return "r * 100000000007 + " + k;
	case 'M':
		return "ROUND(r * 1.000000007 + " + k + " / 3, 9)";
	case 'D':
		return "DATE '2020-01-01' + MOD(r * 13 + " + k + ", 2000) + MOD(r, 86400) / 86400";
	case 'C':
		return "CHR(65 + MOD(r + " + k + ", 26))";
	case 'T':
		return "TIMESTAMP '2020-01-01 00:00:00' + NUMTODSINTERVAL(MOD(r * 7919 + " + k +
		       ", 100000000000) / 1000000, 'SECOND')";
	default:
		return "ROUND(r / (" + k + " + 7), 4)";
	}
}

constexpr int64_t kWideRows = 200000;

// Builds RP_WIDE if needed; the query, its defines (raw bytes: SQLT_CHR, SQLT_NUM, SQLT_DAT, type 180) and the
// define bytes per row as Task 7 counts them (define size + indicator + length)
void WideTable(OciSession &s, std::string &sql, std::vector<ColumnSpec> &columns, uint64_t &row_bytes) {
	std::string ddl, select, fill;
	columns.clear();
	row_bytes = 0;
	for (const auto &group : WideGroups()) {
		for (int j = 1; j <= group.count; j++) {
			const std::string name = WideName(group.prefix, j);
			ddl += (ddl.empty() ? "" : ", ") + name + " " + group.type;
			select += (select.empty() ? "" : ", ") + name;
			fill += (fill.empty() ? "" : ", ") + WideExpr(group.prefix, j);
			columns.push_back(group.dty == kInternalTimestamp ? TimestampColumn(s)
			                                                  : ColumnSpec {group.dty, group.width});
			row_bytes += static_cast<uint64_t>(columns.back().width) + 8;
		}
	}
	Prepare(s, "RP_WIDE", "CREATE TABLE RP_WIDE (" + ddl + ")",
	        {"INSERT /*+ APPEND */ INTO RP_WIDE SELECT " + fill + " FROM " + Rows(kWideRows)}, kWideRows);
	sql = "SELECT " + select + " FROM RP_WIDE";
}

ub4 WideRowsPerFetch(uint64_t budget_bytes, uint64_t row_bytes) {
	return static_cast<ub4>(std::max<uint64_t>(1, std::min<uint64_t>(2048, budget_bytes / row_bytes)));
}

void R3(OciSession &s, StatReader &stat) {
	Section("R3 — 550-column table (200,000 rows): define budget × prefetch memory");
	std::string sql;
	std::vector<ColumnSpec> columns;
	uint64_t row_bytes = 0;
	WideTable(s, sql, columns, row_bytes);
	std::printf("Columns: 200 `VARCHAR2(4000 BYTE)` (5–30 characters, 10 %% NULL), 100 `NUMBER(38,0)`, 80 "
	            "`NUMBER(25,9)`, 60 `DATE`, 50 `CHAR(1 BYTE)`, 30 `TIMESTAMP(6)`, 30 `NUMBER`, fetched as raw bytes "
	            "(`SQLT_CHR`, `SQLT_NUM`, `SQLT_DAT`, type 180: decode costs are in R1, R2 and R7); define buffer %s "
	            "bytes per row (define size + 8). Rows per fetch = budget / bytes per row (at most 2,048). "
	            "`OCI_ATTR_PREFETCH_ROWS = 0`. The sink reads every indicator and length.\n\n",
	            Thousands(static_cast<double>(row_bytes)).c_str());
	struct Config {
		ub4 budget_mib;
		ub4 prefetch_mib;
		ub4 rows;
		std::vector<Run> runs;
		std::string check;
	};
	std::vector<Config> configs;
	for (ub4 budget_mib : {16u, 64u, 256u}) {
		const ub4 rows = WideRowsPerFetch(static_cast<uint64_t>(budget_mib) << 20, row_bytes);
		for (ub4 prefetch_mib : {0u, 4u, 16u, 64u}) {
			configs.push_back({budget_mib, prefetch_mib, rows, {}, "OK"});
		}
	}
	int64_t bytes = 0;
	const Sink sink = [&](Cursor &c, ub4 n) {
		for (size_t col = 0; col < columns.size(); col++) {
			for (ub4 r = 0; r < n; r++) {
				bytes += c.Null(col, r) ? 0 : c.Length(col, r);
			}
		}
	};
	Scan(s, stat, sql, {}, columns, {configs[4].rows, 0, 0}, sink); // warm-up: blocks into the cache
	for (int rep = 0; rep < kRepetitions; rep++) {
		for (auto &config : configs) {
			try {
				const Run run = Scan(s, stat, sql, {}, columns, {config.rows, 0, config.prefetch_mib << 20}, sink);
				if (run.rows != kWideRows) {
					config.check = "FAIL: " + std::to_string(run.rows) + " rows";
				} else if (!run.info.empty() && config.check == "OK") {
					config.check = "OK, info: " + run.info;
				}
				config.runs.push_back(run);
			} catch (const std::exception &e) {
				config.check = std::string("ERROR: ") + e.what();
			}
		}
	}
	std::printf("Median of %d interleaved runs (after one warm-up scan).\n\n", kRepetitions);
	std::printf(
	    "| Budget (MiB) | Prefetch memory (MiB) | Rows/fetch | Wall (s) | Rows/s | Client CPU (s) | Round trips / "
	    "10,000 rows | KB / round trip | Wire (MB/s) | Check |\n|---|---|---|---|---|---|---|---|---|---|\n");
	int64_t wire_bytes = 0, wire_rows = 0;
	for (const auto &config : configs) {
		if (config.runs.empty()) {
			std::printf("| %u | %u | %u | — | — | — | — | — | — | %s |\n", config.budget_mib, config.prefetch_mib,
			            config.rows, config.check.c_str());
			continue;
		}
		const Run m = Median(config.runs);
		wire_bytes += m.stats.bytes_sent;
		wire_rows += m.rows;
		std::printf("| %u | %u | %u | %.2f | %s | %.2f | %s | %s | %s | %s |\n", config.budget_mib, config.prefetch_mib,
		            config.rows, m.wall, Thousands(m.rows / m.wall).c_str(), m.cpu, Fixed(PerTenThousand(m), 1).c_str(),
		            Thousands(m.stats.bytes_sent / 1e3 / std::max<int64_t>(1, m.stats.round_trips)).c_str(),
		            Fixed(m.stats.bytes_sent / m.wall / 1e6, 0).c_str(), config.check.c_str());
	}
	if (wire_rows) {
		std::printf("\nBytes sent by Oracle per row: %s (SQL*Net bytes / rows; at 115 MB/s of payload that is %s "
		            "rows/s on a 1 Gbit/s link). Fetches are synchronous: with a 0.5 ms round-trip time (spec 10.3), "
		            "90 %% of 125 MB/s needs at least 563 KB per round trip.\n",
		            Thousands(static_cast<double>(wire_bytes) / wire_rows).c_str(),
		            Thousands(115e6 / (static_cast<double>(wire_bytes) / wire_rows)).c_str());
	}

	// Narrow rows: 2,048 rows per fetch (one DuckDB vector) are few bytes; prefetch memory or a larger define array
	// adds rows per round trip
	PreparePartTable(s);
	const auto narrow = PartColumns(s);
	std::printf(
	    "\nNarrow table: RP_PART of R5 (6 columns: `NUMBER(12)` and `NUMBER(4)` as `SQLT_INT`, `NUMBER(15,2)`, "
	    "`VARCHAR2(30)`, `DATE`, `TIMESTAMP(6)`; 2,000,000 rows; define buffer 134 bytes per row), median of %d "
	    "interleaved runs.\n\n",
	    kRepetitions);
	std::printf(
	    "| Rows/fetch | Prefetch memory (MiB) | Wall (s) | Rows/s | Client CPU (s) | Round trips / 10,000 rows | KB / "
	    "round trip |\n|---|---|---|---|---|---|---|\n");
	const std::vector<std::pair<ub4, ub4>> narrow_configs = {{2048, 0},  {2048, 4}, {2048, 16},
	                                                         {2048, 64}, {8192, 0}, {32768, 0}};
	std::vector<std::vector<Run>> narrow_runs(narrow_configs.size());
	const Sink none = [](Cursor &, ub4) {
	};
	Scan(s, stat, PartSql("", Slice::kAll, false), {}, narrow, {}, none); // warm-up
	for (int rep = 0; rep < kRepetitions; rep++) {
		for (size_t k = 0; k < narrow_configs.size(); k++) {
			narrow_runs[k].push_back(Scan(s, stat, PartSql("", Slice::kAll, false), {}, narrow,
			                              {narrow_configs[k].first, 0, narrow_configs[k].second << 20}, none));
		}
	}
	for (size_t k = 0; k < narrow_configs.size(); k++) {
		const Run m = Median(narrow_runs[k]);
		std::printf("| %s | %u | %.3f | %s | %.3f | %s | %s |\n", Thousands(narrow_configs[k].first).c_str(),
		            narrow_configs[k].second, m.wall, Thousands(m.rows / m.wall).c_str(), m.cpu,
		            Fixed(PerTenThousand(m), 2).c_str(),
		            Thousands(m.stats.bytes_sent / 1e3 / std::max<int64_t>(1, m.stats.round_trips)).c_str());
	}
}

// --- R4 CLOBs ---

const char *const kMultiByte = "\xC3\xA9\xE6\xBC\xA2\xF0\x9F\x98\x80"; // é 漢 😀: 2, 3 and 4 bytes in UTF-8
const char *const kPadSql = "'abcdefgh\xC3\xA9'";                      // RPAD pad: 9 characters, 10 bytes

// RPAD(head, chars, 'abcdefghé') as Oracle builds it (one-character head)
std::string Rpad(const std::string &head, size_t chars) {
	static const char *const pad[] = {"a", "b", "c", "d", "e", "f", "g", "h", "\xC3\xA9"};
	std::string out = head;
	for (size_t k = 0; k + 1 < chars; k++) {
		out += pad[k % 9];
	}
	return out;
}

// Value of row id in the small (all <= 4,000 bytes) and mixed (every 10th row ~100 KB) distributions
size_t SmallChars(int64_t id) {
	return 1 + static_cast<size_t>((id * 7919) % 3600);
}

const std::string &BigClob() { // 25 times a 3,600-character, 3,999-byte piece: 99,975 bytes
	static const std::string value = [] {
		std::string out;
		const std::string piece = Rpad("y", 3600);
		for (int k = 0; k < 25; k++) {
			out += piece;
		}
		return out;
	}();
	return value;
}

std::string Pieces(const std::string &tag, int count) {
	std::string out;
	for (int k = 1; k <= count; k++) {
		out += tag + std::to_string(k) + kMultiByte + ";";
	}
	return out;
}

// Rows of RP_CLOB_BIG: id -> expected value (nullptr = NULL)
struct BigRow {
	int64_t id;
	std::string label;
	bool null;
	std::string value;
};

const std::vector<BigRow> &BigRows() {
	static const std::vector<BigRow> rows = {{1, "1.4 MB", false, Pieces("a", 90000)},
	                                         {2, "3.0 MB", false, Pieces("bb", 170000)},
	                                         {3, "NULL", true, ""},
	                                         {4, "EMPTY_CLOB()", false, ""},
	                                         {5, "é漢😀 (9 bytes)", false, kMultiByte}};
	return rows;
}

void PrepareClobs(OciSession &s) {
	constexpr int64_t kClobRows = 100000;
	const std::string small = "TO_CLOB(RPAD('x', 1 + MOD(r * 7919, 3600), " + std::string(kPadSql) + "))";
	Prepare(s, "RP_CLOB_SMALL", "CREATE TABLE RP_CLOB_SMALL (ID NUMBER, C CLOB)",
	        {"INSERT /*+ APPEND */ INTO RP_CLOB_SMALL SELECT r, " + small + " FROM " + Rows(kClobRows)}, kClobRows);
	Prepare(
	    s, "RP_CLOB_MIXED", "CREATE TABLE RP_CLOB_MIXED (ID NUMBER, C CLOB)",
	    {"DECLARE big CLOB; BEGIN DBMS_LOB.CREATETEMPORARY(big, TRUE); FOR k IN 1 .. 25 LOOP "
		 "DBMS_LOB.APPEND(big, TO_CLOB(RPAD('y', 3600, " +
		 std::string(kPadSql) +
		 "))); END LOOP; INSERT /*+ APPEND */ INTO RP_CLOB_MIXED SELECT r, CASE WHEN MOD(r, 10) = 0 THEN big ELSE " +
		 small + " END FROM " + Rows(kClobRows) + "; COMMIT; DBMS_LOB.FREETEMPORARY(big); END;"},
	    kClobRows);
	Prepare(s, "RP_CLOB_BIG", "CREATE TABLE RP_CLOB_BIG (ID NUMBER, C CLOB)",
	        {"DECLARE PROCEDURE put_value(p_id NUMBER, p_tag VARCHAR2, p_count PLS_INTEGER) IS c CLOB; "
	         "buf VARCHAR2(32767); BEGIN DBMS_LOB.CREATETEMPORARY(c, TRUE); FOR k IN 1 .. p_count LOOP "
	         "buf := buf || p_tag || TO_CHAR(k) || '" +
	         std::string(kMultiByte) +
	         ";'; IF LENGTHB(buf) > 32000 THEN DBMS_LOB.APPEND(c, TO_CLOB(buf)); buf := NULL; END IF; END LOOP; "
	         "IF buf IS NOT NULL THEN DBMS_LOB.APPEND(c, TO_CLOB(buf)); END IF; "
	         "INSERT INTO RP_CLOB_BIG (ID, C) VALUES (p_id, c); DBMS_LOB.FREETEMPORARY(c); END; "
	         "BEGIN put_value(1, 'a', 90000); put_value(2, 'bb', 170000); "
	         "INSERT INTO RP_CLOB_BIG (ID, C) VALUES (3, NULL); "
	         "INSERT INTO RP_CLOB_BIG (ID, C) VALUES (4, EMPTY_CLOB()); "
	         "INSERT INTO RP_CLOB_BIG (ID, C) VALUES (5, TO_CLOB('" +
	         std::string(kMultiByte) + "')); END;"},
	        static_cast<int64_t>(BigRows().size()));
}

enum class LobTechnique { kDynamic, kPrefetch, kSubstr };

struct LobVariant {
	std::string label;
	LobTechnique technique;
	ub4 prefetch; // characters (lob_prefetch)
	ub2 dty;      // external type (dynamic_define)
};

constexpr int64_t kSubstrChars = 1333; // 1,333 UTF-16 code units always fit 4,000 bytes of UTF-8

using LobCheck = std::function<void(int64_t id, const uint8_t *value, size_t length)>; // value nullptr = NULL

// Reads ID and the CLOB C of every row of table with one technique
Run ReadClobs(OciSession &s, StatReader &stat, const std::string &table, const LobVariant &v, const LobCheck &check) {
	std::string sql = "SELECT ID, C FROM " + table;
	std::vector<ColumnSpec> columns = {{SQLT_INT, 8}};
	switch (v.technique) {
	case LobTechnique::kDynamic:
		columns.push_back({v.dty, 0, 0, 0, true});
		break;
	case LobTechnique::kPrefetch:
		columns.push_back({SQLT_CLOB, 0, OCI_DTYPE_LOB, v.prefetch});
		break;
	case LobTechnique::kSubstr:
		sql = "SELECT ID, DBMS_LOB.SUBSTR(C, " + std::to_string(kSubstrChars) + ", 1), DBMS_LOB.GETLENGTH(C), C FROM " +
		      table;
		columns.push_back({SQLT_CHR, 4000});
		columns.push_back({SQLT_INT, 8});
		columns.push_back({SQLT_CLOB, 0, OCI_DTYPE_LOB, 0});
		break;
	}
	std::string scratch;
	std::vector<uint8_t> buffer;
	// Whole value through the locator (chars = its length in characters)
	auto read_locator = [&](void *locator, ub8 chars, size_t &length) -> const uint8_t * {
		length = 0;
		if (chars == 0) {
			return kEmpty;
		}
		const size_t capacity = static_cast<size_t>(chars) * 4;
		if (buffer.size() < capacity) {
			buffer.resize(capacity);
		}
		ub8 bytes = 0, amount = chars;
		CheckOci(Oci().OCILobRead2(s.svc(), s.err(), static_cast<OCILobLocator *>(locator), &bytes, &amount, 1,
		                           buffer.data(), capacity, OCI_ONE_PIECE, nullptr, nullptr, 0, SQLCS_IMPLICIT),
		         s.err(), "OCILobRead2");
		length = static_cast<size_t>(bytes);
		return buffer.data();
	};
	return Scan(s, stat, sql, {}, columns, {1024, 0, 0}, [&](Cursor &c, ub4 n) {
		for (ub4 r = 0; r < n; r++) {
			const int64_t id = c.Int(0, r);
			const uint8_t *value = nullptr;
			size_t length = 0;
			switch (v.technique) {
			case LobTechnique::kDynamic:
				if (!c.Null(1, r)) {
					value = c.Pieces(1, r, length, scratch);
				}
				break;
			case LobTechnique::kPrefetch:
				if (!c.Null(1, r)) {
					ub8 chars = 0;
					CheckOci(Oci().OCILobGetLength2(s.svc(), s.err(), static_cast<OCILobLocator *>(c.Descriptor(1, r)),
					                                &chars),
					         s.err(), "OCILobGetLength2");
					value = read_locator(c.Descriptor(1, r), chars, length);
				}
				break;
			case LobTechnique::kSubstr:
				if (!c.Null(2, r)) {
					const int64_t chars = c.Int(2, r);
					if (chars <= kSubstrChars) { // inline (DBMS_LOB.SUBSTR of an empty CLOB is NULL)
						value = c.Null(1, r) ? kEmpty : c.Bytes(1, r);
						length = c.Null(1, r) ? 0 : c.Length(1, r);
					} else {
						value = read_locator(c.Descriptor(3, r), static_cast<ub8>(chars), length);
					}
				}
				break;
			}
			check(id, value, length);
		}
	});
}

std::string PerThousand(int64_t count, int64_t rows) {
	return count < 0 ? "—"
	                 : Fixed(1000.0 * static_cast<double>(count) / static_cast<double>(std::max<int64_t>(1, rows)), 1);
}

void R4(OciSession &s, StatReader &stat) {
	Section("R4 — CLOB into VARCHAR (100,000 rows per distribution)");
	PrepareClobs(s);
	const std::vector<LobVariant> variants = {
	    {"`dynamic_define` as specified (`SQLT_LVC`, `OCI_DYNAMIC_FETCH`)", LobTechnique::kDynamic, 0, SQLT_LVC},
	    {"`dynamic_define` with `SQLT_CHR` (`OCI_DYNAMIC_FETCH`, 64 KiB pieces)", LobTechnique::kDynamic, 0, SQLT_CHR},
	    {"`lob_prefetch` (locators, LOB prefetch 4,000 characters)", LobTechnique::kPrefetch, 4000, 0},
	    {"`lob_prefetch` (locators, LOB prefetch 16,384 characters)", LobTechnique::kPrefetch, 16384, 0},
	    {"`lob_prefetch` (locators, LOB prefetch 32,768 characters)", LobTechnique::kPrefetch, 32768, 0},
	    {"`lob_prefetch` (locators, LOB prefetch 131,072 characters)", LobTechnique::kPrefetch, 131072, 0},
	    {"`substr_locator` (`DBMS_LOB.SUBSTR` ≤ 1,333 characters inline, else locator)", LobTechnique::kSubstr, 0, 0}};
	std::printf("Small: `RPAD('x', 1 + MOD(id * 7919, 3600), 'abcdefghé')`, 1 to 3,999 bytes (mean ~2,000), ASCII "
	            "and `é`. Mixed: the same with every 10th row 99,975 bytes (90,000 characters). 1,024 rows per fetch, "
	            "prefetch off (`OCI_ATTR_PREFETCH_ROWS = 0`, `OCI_ATTR_PREFETCH_MEMORY = 0`); median of %d runs. "
	            "Requests: data segments the client sent to Oracle (kernel TCP counters, one per OCI call); `V$MYSTAT` "
	            "misses LOB reads served after a LOB prefetch, so both are shown. Check: every value compared byte for "
	            "byte with the generator.\n\n",
	            kRepetitions);
	struct Cell {
		std::vector<Run> runs;
		std::string check = "OK";
		bool failed = false;
	};
	const std::vector<std::string> tables = {"RP_CLOB_SMALL", "RP_CLOB_MIXED"};
	std::vector<std::vector<Cell>> cells(variants.size(), std::vector<Cell>(tables.size()));
	std::vector<std::string> small_cache(3601);
	for (int rep = 0; rep < kRepetitions; rep++) {
		for (size_t v = 0; v < variants.size(); v++) {
			for (size_t t = 0; t < tables.size(); t++) {
				Cell &cell = cells[v][t];
				if (cell.failed) {
					continue;
				}
				const bool mixed = tables[t] == "RP_CLOB_MIXED";
				int64_t bad = 0, first_bad = -1, bytes = 0;
				try {
					Run run = ReadClobs(s, stat, tables[t], variants[v],
					                    [&](int64_t id, const uint8_t *value, size_t length) {
						                    const std::string *expected;
						                    if (mixed && id % 10 == 0) {
							                    expected = &BigClob();
						                    } else {
							                    std::string &cached = small_cache[SmallChars(id)];
							                    if (cached.empty()) {
								                    cached = Rpad("x", SmallChars(id));
							                    }
							                    expected = &cached;
						                    }
						                    bytes += static_cast<int64_t>(length);
						                    if (!value || length != expected->size() ||
											    std::memcmp(value, expected->data(), length) != 0) {
							                    bad++;
							                    if (first_bad < 0) {
								                    first_bad = id;
							                    }
						                    }
					                    });
					run.values = bytes; // payload bytes
					if (bad || run.rows != 100000) {
						cell.check = "FAIL: " + std::to_string(bad) + " values differ (first id " +
						             std::to_string(first_bad) + "), " + std::to_string(run.rows) + " rows";
					}
					cell.runs.push_back(run);
				} catch (const std::exception &e) {
					cell.check = std::string("ERROR: ") + e.what();
					cell.failed = true;
				}
			}
		}
	}
	std::printf(
	    "| Technique | Distribution | Wall (s) | Rows/s | Payload (MB/s) | Client CPU (s) | Requests / 1,000 rows | "
	    "`V$MYSTAT` round trips / 1,000 rows | Check |\n|---|---|---|---|---|---|---|---|---|\n");
	for (size_t v = 0; v < variants.size(); v++) {
		for (size_t t = 0; t < tables.size(); t++) {
			const Cell &cell = cells[v][t];
			const char *distribution = t ? "mixed" : "small";
			if (cell.runs.empty()) {
				std::printf("| %s | %s | — | — | — | — | — | — | %s |\n", variants[v].label.c_str(), distribution,
				            cell.check.c_str());
				continue;
			}
			const Run m = Median(cell.runs);
			std::printf("| %s | %s | %.2f | %s | %s | %.2f | %s | %s | %s |\n", variants[v].label.c_str(), distribution,
			            m.wall, Thousands(m.rows / m.wall).c_str(), Fixed(m.values / m.wall / 1e6, 0).c_str(), m.cpu,
			            PerThousand(m.stats.requests, m.rows).c_str(), PerThousand(m.stats.round_trips, m.rows).c_str(),
			            cell.check.c_str());
		}
	}
	std::fflush(stdout);

	std::string labels;
	for (const auto &row : BigRows()) {
		labels += (labels.empty() ? "" : ", ") + row.label;
	}
	std::printf("\nExact check of large and edge values (%s; the large values repeat `<tag><k>é漢😀;`, so "
	            "multi-byte characters straddle every piece and chunk boundary):\n\n",
	            labels.c_str());
	std::printf("| Technique | Check | Requests (5 rows) | `V$MYSTAT` round trips |\n|---|---|---|---|\n");
	for (const auto &v : variants) {
		std::string failures;
		int ok = 0;
		try {
			const Run run = ReadClobs(s, stat, "RP_CLOB_BIG", v, [&](int64_t id, const uint8_t *value, size_t length) {
				for (const auto &row : BigRows()) {
					if (row.id != id) {
						continue;
					}
					if (row.null ? value == nullptr
					             : value && length == row.value.size() &&
					                   std::memcmp(value, row.value.data(), length) == 0) {
						ok++;
						continue;
					}
					size_t diff = 0;
					while (value && diff < length && diff < row.value.size() &&
					       value[diff] == uint8_t(row.value[diff])) {
						diff++;
					}
					std::string bytes;
					for (size_t k = diff; value && k < length && k < diff + 6; k++) {
						char hex[4];
						std::snprintf(hex, sizeof(hex), "%02x", value[k]);
						bytes += (bytes.empty() ? "" : " ") + std::string(hex);
					}
					failures += "; " + row.label + ": " +
					            (value ? std::to_string(length) + " bytes instead of " +
					                         std::to_string(row.value.size()) + ", first difference at byte " +
					                         std::to_string(diff) + (bytes.empty() ? "" : " (`" + bytes + "`)")
					                   : std::string("NULL"));
				}
			});
			const std::string total = std::to_string(BigRows().size());
			const std::string check = ok == static_cast<int>(BigRows().size())
			                              ? "OK (" + std::to_string(ok) + "/" + total + ")"
			                              : "FAIL (" + std::to_string(ok) + "/" + total + ")" + failures;
			std::printf("| %s | %s | %s | %s |\n", v.label.c_str(), check.c_str(),
			            run.stats.requests < 0 ? "—" : Thousands(static_cast<double>(run.stats.requests)).c_str(),
			            Thousands(static_cast<double>(run.stats.round_trips)).c_str());
		} catch (const std::exception &e) {
			std::printf("| %s | ERROR: %s | — | — |\n", v.label.c_str(), e.what());
		}
		std::fflush(stdout);
	}
}

// --- R5/R6 partitioned table ---

constexpr int64_t kPartRows = 2000000;
constexpr int kPartitions = 8;
constexpr int kSessions = 4;
const char *const kPartFilter = "PART_KEY = 5";

void PreparePartTable(OciSession &s) {
	std::string partitions;
	for (int p = 1; p <= kPartitions; p++) {
		partitions += (p > 1 ? ", " : "") + std::string("PARTITION P") + std::to_string(p) + " VALUES (" +
		              std::to_string(p) + ")";
	}
	Prepare(s, "RP_PART",
	        "CREATE TABLE RP_PART (ID NUMBER(12), PART_KEY NUMBER(4), N1 NUMBER(15,2), V1 VARCHAR2(30), D1 DATE, "
	        "T1 TIMESTAMP(6)) PARTITION BY LIST (PART_KEY) (" +
	            partitions + ")",
	        {"INSERT /*+ APPEND */ INTO RP_PART SELECT r, MOD(r, 8) + 1, ROUND(r * 1.37, 2), 'label_' || "
	         "MOD(r * 7919, 100000), DATE '2020-01-01' + MOD(r, 1000), TIMESTAMP '2020-01-01 00:00:00' + "
	         "NUMTODSINTERVAL(MOD(r * 7919, 86400000000) / 1000000, 'SECOND') FROM " +
	             Rows(kPartRows),
	         "BEGIN DBMS_STATS.GATHER_TABLE_STATS(USER, 'RP_PART'); END;"},
	        kPartRows);
}

std::vector<ColumnSpec> PartColumns(OciSession &s) {
	return {{SQLT_INT, 8}, {SQLT_INT, 8}, {SQLT_NUM, 22}, {SQLT_CHR, 30}, {SQLT_DAT, 7}, TimestampColumn(s)};
}

// Unit query; binds: SCN (as_of), then lo/hi (kRowid: half-open, kRowidBetween: closed) or k (kHash)
std::string PartSql(const std::string &filter, Slice slice, bool as_of) {
	std::string sql = "SELECT ID, PART_KEY, N1, V1, D1, T1 FROM RP_PART";
	int bind = 1;
	if (as_of) {
		sql += " AS OF SCN TO_NUMBER(:" + std::to_string(bind++) + ")";
	}
	std::vector<std::string> where;
	if (!filter.empty()) {
		where.push_back(filter);
	}
	if (slice == Slice::kRowid) {
		where.push_back("ROWID >= CHARTOROWID(:" + std::to_string(bind) +
		                ") AND ROWID < CHARTOROWID(:" + std::to_string(bind + 1) + ")");
	} else if (slice == Slice::kRowidBetween) {
		where.push_back("ROWID BETWEEN CHARTOROWID(:" + std::to_string(bind) +
		                ") AND CHARTOROWID(:" + std::to_string(bind + 1) + ")");
	} else if (slice == Slice::kHash) {
		where.push_back("ORA_HASH(ROWID, " + std::to_string(kSessions - 1) + ") = TO_NUMBER(:" + std::to_string(bind) +
		                ")");
	}
	for (size_t i = 0; i < where.size(); i++) {
		sql += (i ? " AND " : " WHERE ") + where[i];
	}
	return sql;
}

struct PartUnit {
	Slice slice;
	std::vector<std::string> binds;
	std::string partition;
	uint64_t blocks = 0;
};

struct UnitPlan {
	std::vector<PartUnit> units;
	std::string source;  // where the extent map came from, and why not elsewhere
	double seconds = 0;  // planning time (dictionary queries and calls)
	size_t extents = 0;  // extents or chunks read
	uint64_t target = 0; // blocks per unit
};

// ROWID units over every segment of RP_PART. USER_EXTENTS has no block address (no RELATIVE_FNO, BLOCK_ID),
// so the extent map comes from DBA_EXTENTS (Task 5's rule: per object, cut a unit when the target is reached;
// consecutive units share their boundary; the last one ends at object + 1), or, when DBA_EXTENTS is not
// readable, from DBMS_PARALLEL_EXECUTE.CREATE_CHUNKS_BY_ROWID (definer's rights; needs SELECT on the table).
UnitPlan PlanRowidUnits(OciSession &s) {
	UnitPlan plan;
	const uint64_t total =
	    std::stoull(Scalar(s, "SELECT TO_CHAR(SUM(blocks)) FROM user_segments WHERE segment_name = 'RP_PART'"));
	plan.target = std::max<uint64_t>((total + 4 * kSessions - 1) / (4 * kSessions), 128);
	try {
		std::vector<std::vector<std::string>> extents;
		plan.seconds =
		    Measure([&] {
			    extents = s.QueryStrings(
			        "SELECT o.data_object_id, o.subobject_name, e.blocks, "
			        "ROWIDTOCHAR(DBMS_ROWID.ROWID_CREATE(1, o.data_object_id, CASE WHEN e.relative_fno = 1024 "
			        "THEN TRUNC(e.block_id / 4194304) ELSE e.relative_fno END, CASE WHEN e.relative_fno = 1024 "
			        "THEN MOD(e.block_id, 4194304) ELSE e.block_id END, 0)), "
			        "ROWIDTOCHAR(DBMS_ROWID.ROWID_CREATE(1, o.data_object_id, 0, 0, 0)), "
			        "ROWIDTOCHAR(DBMS_ROWID.ROWID_CREATE(1, o.data_object_id + 1, 0, 0, 0)) "
			        "FROM dba_extents e JOIN all_objects o ON o.owner = e.owner AND o.object_name = "
			        "e.segment_name AND NVL(o.subobject_name, ' ') = NVL(e.partition_name, ' ') AND "
			        "o.object_type LIKE 'TABLE%' WHERE e.owner = USER AND e.segment_name = 'RP_PART' AND "
			        "e.segment_type IN ('TABLE', 'TABLE PARTITION', 'TABLE SUBPARTITION') "
			        "ORDER BY o.data_object_id, e.relative_fno, e.block_id");
		    }).wall;
		plan.extents = extents.size();
		for (size_t i = 0; i < extents.size();) {
			size_t end = i;
			while (end < extents.size() && extents[end][0] == extents[i][0]) {
				end++;
			}
			PartUnit unit {Slice::kRowid, {extents[i][4], ""}, extents[i][1], 0};
			for (size_t k = i; k < end; k++) {
				if (unit.blocks >= plan.target) {
					unit.binds[1] = extents[k][3];
					plan.units.push_back(unit);
					unit = PartUnit {Slice::kRowid, {extents[k][3], ""}, extents[i][1], 0};
				}
				unit.blocks += std::stoull(extents[k][2]);
			}
			unit.binds[1] = extents[i][5];
			plan.units.push_back(unit);
			i = end;
		}
		std::stable_sort(plan.units.begin(), plan.units.end(),
		                 [](const PartUnit &a, const PartUnit &b) { return a.blocks > b.blocks; });
		plan.source = "`DBA_EXTENTS` ⨝ `ALL_OBJECTS` with `DBMS_ROWID.ROWID_CREATE`";
		return plan;
	} catch (const OracleError &e) {
		plan.source = "`DBA_EXTENTS`: ORA-" + std::to_string(e.ora_code) + ", so ";
	}
	std::vector<std::vector<std::string>> chunks;
	plan.seconds =
	    Measure([&] {
		    s.Execute("BEGIN BEGIN DBMS_PARALLEL_EXECUTE.DROP_TASK('ORADUCK_PROBE'); EXCEPTION WHEN OTHERS "
			          "THEN NULL; END; DBMS_PARALLEL_EXECUTE.CREATE_TASK('ORADUCK_PROBE'); "
			          "DBMS_PARALLEL_EXECUTE.CREATE_CHUNKS_BY_ROWID('ORADUCK_PROBE', USER, 'RP_PART', FALSE, " +
			          std::to_string(plan.target) + "); END;");
		    chunks = s.QueryStrings(
		        "SELECT ROWIDTOCHAR(c.start_rowid), ROWIDTOCHAR(c.end_rowid), o.subobject_name FROM "
		        "user_parallel_execute_chunks c JOIN user_objects o ON o.data_object_id = "
		        "DBMS_ROWID.ROWID_OBJECT(c.start_rowid) AND o.object_name = 'RP_PART' WHERE c.task_name = "
		        "'ORADUCK_PROBE' ORDER BY c.chunk_id");
		    s.Execute("BEGIN DBMS_PARALLEL_EXECUTE.DROP_TASK('ORADUCK_PROBE'); END;");
	    }).wall;
	plan.extents = chunks.size();
	for (const auto &chunk : chunks) {
		plan.units.push_back({Slice::kRowidBetween, {chunk[0], chunk[1]}, chunk[2], 0});
	}
	plan.source += "`DBMS_PARALLEL_EXECUTE.CREATE_CHUNKS_BY_ROWID` (chunk size = target blocks, then "
	               "`ROWID BETWEEN start_rowid AND end_rowid`; create task + chunks + read + drop task)";
	return plan;
}

struct ParallelRun {
	double wall = 0;
	int64_t rows = 0;
	int64_t id_sum = 0;
	std::vector<double> unit_seconds;
	std::string error;
};

// Units from a shared queue; each thread owns one session
ParallelRun RunParallel(std::vector<std::unique_ptr<OciSession>> &sessions, size_t session_count,
                        const std::string &filter, const std::vector<PartUnit> &units, const std::string &scn) {
	ParallelRun out;
	out.unit_seconds.resize(units.size());
	std::atomic<size_t> next {0};
	std::atomic<int64_t> rows {0}, sum {0};
	std::vector<std::string> errors(session_count);
	const auto columns = PartColumns(*sessions[0]);
	out.wall = Measure([&] {
		           std::vector<std::thread> threads;
		           for (size_t k = 0; k < session_count; k++) {
			           threads.emplace_back([&, k] {
				           try {
					           for (size_t u; (u = next++) < units.size();) {
						           std::vector<std::string> binds;
						           if (!scn.empty()) {
							           binds.push_back(scn);
						           }
						           binds.insert(binds.end(), units[u].binds.begin(), units[u].binds.end());
						           const auto start = std::chrono::steady_clock::now();
						           int64_t unit_rows = 0, unit_sum = 0;
						           Cursor cursor(*sessions[k], PartSql(filter, units[u].slice, !scn.empty()), binds,
								                 columns, {});
						           for (ub4 n; (n = cursor.Fetch()) > 0;) {
							           for (ub4 r = 0; r < n; r++) {
								           unit_sum += cursor.Int(0, r);
							           }
							           unit_rows += n;
						           }
						           out.unit_seconds[u] = Since(start);
						           rows += unit_rows;
						           sum += unit_sum;
					           }
				           } catch (const std::exception &e) {
					           errors[k] = e.what();
				           }
			           });
		           }
		           for (auto &thread : threads) {
			           thread.join();
		           }
	           }).wall;
	out.rows = rows;
	out.id_sum = sum;
	for (const auto &e : errors) {
		if (!e.empty()) {
			out.error = e;
		}
	}
	return out;
}

// Expected row count and SUM(ID) (IDs 1..kPartRows, PART_KEY = MOD(ID, 8) + 1)
void PartExpected(bool filtered, int64_t &count, int64_t &sum) {
	count = sum = 0;
	for (int64_t id = 1; id <= kPartRows; id++) {
		if (!filtered || id % 8 + 1 == 5) {
			count++;
			sum += id;
		}
	}
}

void R5(OciSession &s, StatReader &) {
	Section("R5 — Parallel split of a LIST-partitioned table (2,000,000 rows, 8 partitions, 4 sessions)");
	PreparePartTable(s);
	std::vector<std::unique_ptr<OciSession>> sessions;
	for (int k = 0; k < kSessions; k++) {
		sessions.emplace_back(new OciSession(Credentials()));
	}
	const UnitPlan plan = PlanRowidUnits(s);
	const std::vector<PartUnit> &units = plan.units;
	std::vector<PartUnit> hash_units, one_unit = {{Slice::kAll, {}, "", 0}};
	for (int k = 0; k < kSessions; k++) {
		hash_units.push_back({Slice::kHash, {std::to_string(k)}, "", 0});
	}
	RunParallel(sessions, 1, "", one_unit, ""); // warm-up: blocks into the cache
	std::printf(
	    "Table in the bigfile `BENCH` tablespace. `USER_EXTENTS` has no block address (`RELATIVE_FNO`, "
	    "`BLOCK_ID`), so ROWID ranges cannot come from it. Extent map: %s — %.1f ms for %zu extents/chunks → "
	    "%zu ROWID units (target = max(⌈%s blocks / 16⌉, 128) = %s blocks). Units come from a shared queue; "
	    "sessions are opened before the clock starts. 2,048 rows per fetch, prefetch off; median of %d runs.\n\n",
	    plan.source.c_str(), plan.seconds * 1e3, plan.extents, units.size(),
	    Thousands(static_cast<double>(plan.target * 4 * kSessions)).c_str(),
	    Thousands(static_cast<double>(plan.target)).c_str(), kRepetitions);
	std::printf("| Scan | Strategy | Sessions | Units | Wall (s) | Rows/s | Planning / wall | Check |\n"
	            "|---|---|---|---|---|---|---|---|\n");
	for (const std::string filter : {"", kPartFilter}) {
		int64_t expected_count = 0, expected_sum = 0;
		PartExpected(!filter.empty(), expected_count, expected_sum);
		struct Strategy {
			std::string label;
			size_t sessions;
			const std::vector<PartUnit> *units;
		};
		const std::vector<Strategy> strategies = {{"one session", 1, &one_unit},
		                                          {"extents (ROWID ranges)", kSessions, &units},
		                                          {"hash (`ORA_HASH(ROWID, 3) = k`)", kSessions, &hash_units}};
		std::vector<std::vector<double>> walls(strategies.size());
		std::vector<std::string> checks(strategies.size(), "OK");
		std::vector<int64_t> rows(strategies.size(), 0);
		for (int rep = 0; rep < kRepetitions; rep++) {
			for (size_t k = 0; k < strategies.size(); k++) {
				const ParallelRun run = RunParallel(sessions, strategies[k].sessions, filter, *strategies[k].units, "");
				walls[k].push_back(run.wall);
				rows[k] = run.rows;
				if (!run.error.empty()) {
					checks[k] = "ERROR: " + run.error;
				} else if (run.rows != expected_count || run.id_sum != expected_sum) {
					checks[k] =
					    "FAIL: " + std::to_string(run.rows) + " rows, expected " + std::to_string(expected_count);
				}
			}
		}
		for (size_t k = 0; k < strategies.size(); k++) {
			std::sort(walls[k].begin(), walls[k].end());
			const double wall = walls[k][walls[k].size() / 2];
			std::printf("| %s | %s | %zu | %zu | %.3f | %s | %s | %s |\n",
			            filter.empty() ? "full table" : filter.c_str(), strategies[k].label.c_str(),
			            strategies[k].sessions, strategies[k].units->size(), wall, Thousands(rows[k] / wall).c_str(),
			            strategies[k].units == &units ? (Fixed(100 * plan.seconds / wall, 1) + " %").c_str() : "—",
			            checks[k].c_str());
		}
		std::fflush(stdout);
	}
	// Units alone in one session, with the filter: those of pruned partitions should cost nothing
	double pruned_total = 0, pruned_max = 0, kept_total = 0;
	int pruned = 0, kept = 0;
	int64_t pruned_rows = 0;
	for (const auto &unit : units) {
		const ParallelRun run = RunParallel(sessions, 1, kPartFilter, {unit}, "");
		if (unit.partition == "P5") {
			kept_total += run.wall;
			kept++;
		} else {
			pruned_total += run.wall;
			pruned_max = std::max(pruned_max, run.wall);
			pruned_rows += run.rows;
			pruned++;
		}
	}
	std::printf("\nUnits run alone with `%s` (one session, query included): %d units in pruned partitions: mean %.2f "
	            "ms, max %.2f ms, %lld rows; %d units in partition P5: mean %.1f ms.\n",
	            kPartFilter, pruned, pruned ? 1e3 * pruned_total / pruned : 0.0, 1e3 * pruned_max,
	            static_cast<long long>(pruned_rows), kept, kept ? 1e3 * kept_total / kept : 0.0);
}

// --- R6 AS OF SCN ---

std::string ReadScn(OciSession &s, std::string &source) {
	const std::vector<std::pair<std::string, std::string>> sources = {
	    {"DBMS_FLASHBACK.GET_SYSTEM_CHANGE_NUMBER",
		 "SELECT TO_CHAR(DBMS_FLASHBACK.GET_SYSTEM_CHANGE_NUMBER) FROM dual"},
	    {"V$DATABASE.CURRENT_SCN", "SELECT TO_CHAR(current_scn) FROM v$database"},
	    {"TIMESTAMP_TO_SCN(SYSTIMESTAMP)", "SELECT TO_CHAR(TIMESTAMP_TO_SCN(SYSTIMESTAMP)) FROM dual"}};
	std::string failures;
	for (const auto &candidate : sources) {
		try {
			const std::string scn = Scalar(s, candidate.second);
			source = candidate.first + (failures.empty() ? "" : " (" + failures + ")");
			return scn;
		} catch (const OracleError &e) {
			failures += (failures.empty() ? "" : "; ") + candidate.first + ": ORA-" + std::to_string(e.ora_code);
		}
	}
	source = failures;
	return "";
}

void R6(OciSession &s, StatReader &stat) {
	Section("R6 — `AS OF SCN`");
	PreparePartTable(s);
	std::string source;
	const std::string scn = ReadScn(s, source);
	if (scn.empty()) {
		std::printf("No SCN source worked: %s\n", source.c_str());
		return;
	}
	constexpr int kSnapshotRepetitions = 9;
	std::printf("SCN %s read with %s; queries use `AS OF SCN TO_NUMBER(:1)`. Each scan without `AS OF` is followed by "
	            "the same scan with it; cost = median of the per-pair time ratios (robust to load changes on the "
	            "machine). Noise floor: the same pairs with `AS OF` in neither scan. Server CPU (`CPU used by this "
	            "session`, median) and consistent gets from `V$MYSTAT`.\n\n",
	            scn.c_str(), source.c_str());
	std::printf(
	    "| Scan | Pairs | Without: wall (s) | With `AS OF SCN`: wall (s) | Cost | Server CPU without / with (s) | "
	    "Consistent gets without / with | Rows without / with |\n|---|---|---|---|---|---|---|---|\n");
	struct Pair {
		double plain;
		double as_of;
	};
	auto report = [&](const std::string &label, std::vector<Pair> pairs, const Run *a, const Run *b, int64_t rows_a,
	                  int64_t rows_b) {
		std::vector<double> ratios, plain, as_of;
		for (const auto &pair : pairs) {
			ratios.push_back(pair.as_of / pair.plain - 1);
			plain.push_back(pair.plain);
			as_of.push_back(pair.as_of);
		}
		std::sort(ratios.begin(), ratios.end());
		std::sort(plain.begin(), plain.end());
		std::sort(as_of.begin(), as_of.end());
		const size_t mid = pairs.size() / 2;
		std::printf("| %s | %zu | %.4f | %.4f | %+.1f %% | %s | %s | %s / %s |\n", label.c_str(), pairs.size(),
		            plain[mid], as_of[mid], 100 * ratios[mid],
		            a ? (Fixed(a->stats.server_cpu / 100.0, 2) + " / " + Fixed(b->stats.server_cpu / 100.0, 2)).c_str()
		              : "—",
		            a ? (Thousands(static_cast<double>(a->stats.consistent_gets)) + " / " +
		                 Thousands(static_cast<double>(b->stats.consistent_gets)))
		                    .c_str()
		              : "—",
		            Thousands(static_cast<double>(rows_a)).c_str(), Thousands(static_cast<double>(rows_b)).c_str());
		std::fflush(stdout);
	};
	// noise: the second scan of each pair is the first one again (no AS OF)
	auto single = [&](const std::string &label, const std::string &sql, const std::string &as_of_sql,
	                  const std::vector<std::string> &binds, const std::vector<ColumnSpec> &columns,
	                  const FetchSettings &settings, int repetitions, bool noise) {
		const Sink none = [](Cursor &, ub4) {
		};
		std::vector<std::string> as_of_binds = {scn};
		as_of_binds.insert(as_of_binds.end(), binds.begin(), binds.end());
		if (noise) {
			as_of_binds = binds;
		}
		Scan(s, stat, sql, binds, columns, settings, none); // warm-up
		std::vector<Pair> pairs;
		std::vector<Run> plain, as_of;
		for (int rep = 0; rep < repetitions; rep++) {
			plain.push_back(Scan(s, stat, sql, binds, columns, settings, none));
			as_of.push_back(Scan(s, stat, noise ? sql : as_of_sql, as_of_binds, columns, settings, none));
			pairs.push_back({plain.back().wall, as_of.back().wall});
		}
		auto by_cpu = [](const Run &x, const Run &y) {
			return x.stats.server_cpu < y.stats.server_cpu;
		};
		std::sort(plain.begin(), plain.end(), by_cpu); // medians of server CPU (consistent gets do not vary)
		std::sort(as_of.begin(), as_of.end(), by_cpu);
		report(label, pairs, &plain[plain.size() / 2], &as_of[as_of.size() / 2], plain[0].rows, as_of[0].rows);
	};
	{
		// The workload-shaped scan of R3 (64 MiB budget): the throughput cost, fixed costs negligible
		std::string wide_sql;
		std::vector<ColumnSpec> wide_columns;
		uint64_t row_bytes = 0;
		WideTable(s, wide_sql, wide_columns, row_bytes);
		single("RP_WIDE (550 columns, 200,000 rows)", wide_sql, wide_sql + " AS OF SCN TO_NUMBER(:1)", {}, wide_columns,
		       {WideRowsPerFetch(64u << 20, row_bytes), 0, 0}, kRepetitions, false);
	}
	const auto columns = PartColumns(s);
	for (const std::string filter : {"", kPartFilter}) {
		const std::string label = std::string("RP_PART ") + (filter.empty() ? "full table" : filter);
		single(label, PartSql(filter, Slice::kAll, false), PartSql(filter, Slice::kAll, true), {}, columns, {},
		       kSnapshotRepetitions, false);
		single(label + " — noise floor", PartSql(filter, Slice::kAll, false), "", {}, columns, {}, kSnapshotRepetitions,
		       true);
	}
	// The parallel scan of R5 (extents, 4 sessions)
	const UnitPlan plan = PlanRowidUnits(s);
	{
		std::vector<std::unique_ptr<OciSession>> sessions;
		for (int k = 0; k < kSessions; k++) {
			sessions.emplace_back(new OciSession(Credentials()));
		}
		RunParallel(sessions, kSessions, "", plan.units, ""); // warm-up
		const std::string label = "RP_PART full table, extents, " + std::to_string(kSessions) + " sessions (" +
		                          std::to_string(plan.units.size()) + " units)";
		for (bool noise : {false, true}) {
			std::vector<Pair> pairs;
			int64_t plain_rows = 0, as_of_rows = 0;
			for (int rep = 0; rep < kSnapshotRepetitions; rep++) {
				const ParallelRun a = RunParallel(sessions, kSessions, "", plan.units, "");
				const ParallelRun b = RunParallel(sessions, kSessions, "", plan.units, noise ? "" : scn);
				pairs.push_back({a.wall, b.wall});
				plain_rows = a.error.empty() ? a.rows : -1;
				as_of_rows = b.error.empty() ? b.rows : -1;
			}
			report(noise ? label + " — noise floor" : label, pairs, nullptr, nullptr, plain_rows, as_of_rows);
		}
	}
	// Each ROWID unit of a pruned partition, alone, with the filter
	std::vector<Pair> pruned;
	const Sink none = [](Cursor &, ub4) {
	};
	int64_t pruned_rows = 0;
	for (const auto &unit : plan.units) {
		if (unit.partition == "P5") {
			continue;
		}
		std::vector<std::string> binds = {scn};
		binds.insert(binds.end(), unit.binds.begin(), unit.binds.end());
		const Run a = Scan(s, stat, PartSql(kPartFilter, unit.slice, false), unit.binds, columns, {}, none);
		const Run b = Scan(s, stat, PartSql(kPartFilter, unit.slice, true), binds, columns, {}, none);
		pruned.push_back({a.wall, b.wall});
		pruned_rows += a.rows + b.rows;
	}
	if (!pruned.empty()) {
		report("RP_PART ROWID unit of a pruned partition, `" + std::string(kPartFilter) + "` (one pair per unit)",
		       pruned, nullptr, nullptr, pruned_rows, pruned_rows);
	}
}

// --- R7 unconstrained NUMBER ---

// Exact double of an internal NUMBER: its canonical text, then strtod (correctly rounded)
double NumberToDouble(const uint8_t *bytes, size_t len) {
	return std::strtod(DecodeOracleNumberText(bytes, len).c_str(), nullptr);
}

void R7(OciSession &s, StatReader &stat) {
	Section("R7 — Unconstrained NUMBER into DOUBLE (2,000,000 rows)");
	Prepare(s, "RP_UNUM", "CREATE TABLE RP_UNUM (C NUMBER)",
	        {"INSERT /*+ APPEND */ INTO RP_UNUM SELECT CASE MOD(r, 4) WHEN 0 THEN r WHEN 1 THEN ROUND(r / 7, 12) "
	         "WHEN 2 THEN ROUND(r * 1.0001, 4) ELSE -r * 1000000.5 END FROM " +
	         Rows(kNarrowRows)},
	        kNarrowRows);
	const double expected = std::strtod(Scalar(s, "SELECT TO_CHAR(SUM(C)) FROM RP_UNUM").c_str(), nullptr);
	enum class Kind { kDouble, kText, kTextDouble };
	struct Variant {
		std::string label;
		Kind kind;
		ColumnSpec column;
	};
	const std::vector<Variant> variants = {
	    {"`SQLT_BDOUBLE` (8 bytes, OCI conversion)", Kind::kDouble, {SQLT_BDOUBLE, 8}},
	    {"`SQLT_FLT` (8 bytes, OCI conversion)", Kind::kDouble, {SQLT_FLT, 8}},
	    {"`SQLT_NUM` + `DecodeOracleNumberText` + `strtod`", Kind::kTextDouble, {SQLT_NUM, 22}},
	    {"`SQLT_NUM` + `DecodeOracleNumberText` (the `VARCHAR` option)", Kind::kText, {SQLT_NUM, 22}}};
	std::vector<std::vector<Run>> runs(variants.size());
	std::vector<std::string> checks(variants.size(), "OK");
	for (int rep = 0; rep < kRepetitions; rep++) {
		for (size_t v = 0; v < variants.size(); v++) {
			double sum = 0, decode = 0;
			int64_t values = 0;
			Run run = Scan(s, stat, "SELECT C FROM RP_UNUM", {}, {variants[v].column}, {}, [&](Cursor &c, ub4 n) {
				DecodeClock clock(decode);
				for (ub4 r = 0; r < n; r++) {
					if (c.Null(0, r)) {
						continue;
					}
					if (variants[v].kind == Kind::kDouble) {
						double value;
						std::memcpy(&value, c.Bytes(0, r), sizeof(value));
						sum += value;
					} else if (variants[v].kind == Kind::kTextDouble) {
						sum += NumberToDouble(c.Bytes(0, r), c.Length(0, r));
					} else {
						sum += static_cast<double>(DecodeOracleNumberText(c.Bytes(0, r), c.Length(0, r)).size());
					}
					values++;
				}
			});
			run.decode = decode;
			run.values = values;
			if (variants[v].kind != Kind::kText && std::fabs(sum - expected) > 1e-9 * std::fabs(expected)) {
				checks[v] = "FAIL: sum " + Fixed(sum, 4) + ", expected " + Fixed(expected, 4);
			}
			runs[v].push_back(run);
		}
	}
	// Every double against the correctly rounded one (strtod of the exact decimal), bit for bit
	int64_t doubles = 0;
	int64_t differ_bdouble = 0, differ_flt = 0;
	std::string first_bdouble, first_flt;
	auto describe = [](const std::string &exact, double got) {
		char text[128];
		std::snprintf(text, sizeof(text), " (first: %s → %.17g)", exact.c_str(), got);
		return std::string(text);
	};
	Scan(s, stat, "SELECT C, C, C FROM RP_UNUM", {}, {{SQLT_NUM, 22}, {SQLT_BDOUBLE, 8}, {SQLT_FLT, 8}}, {},
	     [&](Cursor &c, ub4 n) {
		     for (ub4 r = 0; r < n; r++) {
			     if (c.Null(0, r)) {
				     continue;
			     }
			     doubles++;
			     const double exact = NumberToDouble(c.Bytes(0, r), c.Length(0, r));
			     double bdouble, flt;
			     std::memcpy(&bdouble, c.Bytes(1, r), 8);
			     std::memcpy(&flt, c.Bytes(2, r), 8);
			     if (std::memcmp(&exact, &bdouble, 8) != 0 && differ_bdouble++ == 0) {
				     first_bdouble = describe(DecodeOracleNumberText(c.Bytes(0, r), c.Length(0, r)), bdouble);
			     }
			     if (std::memcmp(&exact, &flt, 8) != 0 && differ_flt++ == 0) {
				     first_flt = describe(DecodeOracleNumberText(c.Bytes(0, r), c.Length(0, r)), flt);
			     }
		     }
	     });
	auto rounding = [&](int64_t differ, const std::string &first) {
		return differ ? "; FAIL: " + Thousands(static_cast<double>(differ)) + " / " +
		                    Thousands(static_cast<double>(doubles)) + " not correctly rounded" + first
		              : "; correctly rounded (all " + Thousands(static_cast<double>(doubles)) + ")";
	};
	checks[0] += rounding(differ_bdouble, first_bdouble);
	checks[1] += rounding(differ_flt, first_flt);
	checks[2] += "; correctly rounded by construction";
	// The text decoder against TO_CHAR, row by row
	int64_t compared = 0, mismatches = 0;
	std::string first;
	Scan(s, stat, "SELECT C, TO_CHAR(C) FROM RP_UNUM WHERE ROWNUM <= 20000", {}, {{SQLT_NUM, 22}, {SQLT_CHR, 64}}, {},
	     [&](Cursor &c, ub4 n) {
		     for (ub4 r = 0; r < n; r++) {
			     std::string want(reinterpret_cast<const char *>(c.Bytes(1, r)), c.Length(1, r));
			     if (want.rfind(".", 0) == 0) {
				     want = "0" + want;
			     } else if (want.rfind("-.", 0) == 0) {
				     want = "-0" + want.substr(1);
			     }
			     const std::string got = DecodeOracleNumberText(c.Bytes(0, r), c.Length(0, r));
			     compared++;
			     if (got != want) {
				     mismatches++;
				     if (first.empty()) {
					     first = got + " vs " + want;
				     }
			     }
		     }
	     });
	checks[3] = mismatches ? "FAIL: " + std::to_string(mismatches) + " / " + std::to_string(compared) +
	                             " differ from `TO_CHAR` (first: " + first + ")"
	                       : "OK (" + std::to_string(compared) + " rows equal to `TO_CHAR`)";
	std::printf("Values: integers, `ROUND(r / 7, 12)`, `ROUND(r * 1.0001, 4)`, `-r * 1000000.5`. 2,048 rows per "
	            "fetch, prefetch off; median of %d runs. Checks: Σ against Oracle's `SUM(C)` (relative 1e-9), then "
	            "every double against `strtod` of the exact decimal, bit for bit.\n\n",
	            kRepetitions);
	std::printf("| Define | Wall (s) | Rows/s | Client CPU (s) | Decode (ns/value) | Round trips | Check |\n"
	            "|---|---|---|---|---|---|---|\n");
	for (size_t v = 0; v < variants.size(); v++) {
		const Run m = Median(runs[v]);
		std::printf("| %s | %.3f | %s | %.3f | %s | %s | %s |\n", variants[v].label.c_str(), m.wall,
		            Thousands(m.rows / m.wall).c_str(), m.cpu, NsPerValue(m).c_str(),
		            Thousands(static_cast<double>(m.stats.round_trips)).c_str(), checks[v].c_str());
	}
}

} // namespace

int main(int argc, char **argv) {
	std::set<std::string> only;
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		if (arg == "--reuse") {
			g_reuse = true;
		} else {
			only.insert(arg);
		}
	}
	OciSession s(Credentials());
	StatReader stat(s);
	sword major = 0, minor = 0, update = 0, patch = 0, port = 0;
	Oci().OCIClientVersion(&major, &minor, &update, &patch, &port);
	std::printf("# OraDuck read probe results\n\nOracle %s, Instant Client %d.%d.%d.%d; client CPUs pinned by the "
	            "caller. Round trips: `SQL*Net roundtrips to/from client` from `V$MYSTAT`, minus the %lld of each "
	            "reading.\n",
	            Scalar(s, "SELECT version_full FROM product_component_version WHERE ROWNUM = 1").c_str(), major, minor,
	            update, patch, static_cast<long long>(stat.overhead().round_trips));
	std::fflush(stdout);
	const std::vector<std::pair<std::string, void (*)(OciSession &, StatReader &)>> sections = {
	    {"R1", R1}, {"R2", R2}, {"R3", R3}, {"R4", R4}, {"R5", R5}, {"R6", R6}, {"R7", R7}};
	for (const auto &section : sections) {
		if (!only.empty() && !only.count(section.first)) {
			continue;
		}
		try {
			section.second(s, stat);
		} catch (const std::exception &e) {
			std::printf("\nERROR in %s: %s\n", section.first.c_str(), e.what());
		}
		std::fflush(stdout);
	}
	if (!g_reuse) {
		for (const auto &table : g_tables) {
			Drop(s, table);
		}
	}
	return 0;
}

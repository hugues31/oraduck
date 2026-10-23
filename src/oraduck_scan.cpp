#include "oraduck_scan.hpp"
#include "oraduck_scan_filters.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

// OraDuck (OCI) headers after DuckDB
#include "oraduck_secret.hpp"
#include "scan_vectors.hpp"
#include "oraduck/fetch_cursor.hpp"
#include "oraduck/oracle_names.hpp"
#include "oraduck/work_units.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace duckdb {

namespace {

using oraduck::SnapshotMode;
using oraduck::SplitStrategy;

constexpr int64_t kMaxSessions = 1024;
const char *const kNothing = "\xE2\x80\x94"; // em dash

const char *SplitText(SplitStrategy split) {
	switch (split) {
	case SplitStrategy::kAuto:
		return "auto";
	case SplitStrategy::kExtents:
		return "extents";
	case SplitStrategy::kHash:
		return "hash";
	case SplitStrategy::kNone:
		return "none";
	}
	return "";
}

const char *SnapshotText(SnapshotMode snapshot) {
	switch (snapshot) {
	case SnapshotMode::kAuto:
		return "auto";
	case SnapshotMode::kScn:
		return "scn";
	case SnapshotMode::kNone:
		return "none";
	}
	return "";
}

string DisplayName(const oraduck::OracleTableDescription &source) {
	return source.owner + "." + source.name;
}

// Statement text with each placeholder :bN replaced by the SQL literal of its bind (EXPLAIN ANALYZE)
string InlineBinds(const oraduck::SqlStatement &statement) {
	const string &sql = statement.sql;
	string out;
	char quote = 0;
	for (idx_t i = 0; i < sql.size(); i++) {
		const char c = sql[i];
		if (quote) {
			quote = c == quote ? 0 : quote;
			out.push_back(c);
		} else if (c == '"' || c == '\'') {
			quote = c;
			out.push_back(c);
		} else if (c == ':' && i + 2 < sql.size() && sql[i + 1] == 'b' &&
		           std::isdigit(static_cast<unsigned char>(sql[i + 2]))) {
			idx_t j = i + 2;
			idx_t n = 0;
			while (j < sql.size() && std::isdigit(static_cast<unsigned char>(sql[j]))) {
				n = n * 10 + idx_t(sql[j] - '0');
				j++;
			}
			out += n >= 1 && n <= statement.binds.size() ? statement.binds[n - 1].display : sql.substr(i, j - i);
			i = j - 1;
		} else {
			out.push_back(c);
		}
	}
	return out;
}

// The snapshot notes of a work plan ("snapshot: ..."), or all its notes
string SnapshotNotes(const string &notes) {
	string result;
	for (auto &note : StringUtil::Split(notes, "; ")) {
		if (StringUtil::StartsWith(note, "snapshot")) {
			result += (result.empty() ? "" : "; ") + note;
		}
	}
	return result.empty() ? notes : result;
}

// One execution of a scan: the work plan, shared by the threads, which take its units in order
struct OraduckScanGlobalState : public GlobalTableFunctionState {
	std::vector<const oraduck::ReadColumnPlan *> projected; // fetched columns, in select-list order
	std::vector<optional_idx> outputs; // per output column: its index in projected, invalid for a non-Oracle column
	oraduck::ScanSqlInput sql;         // the statement of every unit, without its slice
	oraduck::WorkPlan plan;
	idx_t sessions = 0; // sessions the plan can use: min(sessions option, units)
	double planning_ms = 0;
	string first_sql; // statement of the first unit, binds inlined
	std::atomic<idx_t> next_unit {0};
	// The planning session, handed over to the first thread that needs a session (one logon less)
	std::mutex lock;
	std::unique_ptr<oraduck::OciSession> spare_session;

	idx_t MaxThreads() const override {
		return MaxValue<idx_t>(1, sessions);
	}
};

// One thread of a scan: its own Oracle session, opened when it takes its first unit
struct OraduckScanLocalState : public LocalTableFunctionState {
	std::unique_ptr<oraduck::OciSession> session;
	std::unique_ptr<oraduck::FetchCursor> cursor; // the current unit; declared after the session, released before it
	uint32_t batch_rows = 0;                      // rows of the cursor's last batch
	uint32_t batch_next = 0;                      // first row of that batch not yet returned
	bool finished = false;
};

unique_ptr<FunctionData> OraduckScanBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	ScanOptions options;
	string secret_name;
	for (auto &entry : input.named_parameters) {
		const auto name = entry.first.GetIdentifierName();
		if (StringUtil::Lower(name) == "connection") {
			secret_name = entry.second.IsNull() ? "" : entry.second.ToString();
		} else {
			ApplyScanOption(options, name, entry.second);
		}
	}
	if (secret_name.empty()) {
		throw BinderException("OraDuck: the connection parameter is required (name of an oraduck secret)");
	}
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("OraDuck: oraduck_scan needs the name of an Oracle table or view");
	}
	auto credentials = GetOraduckCredentials(context, secret_name);
	oraduck::OracleTableDescription source;
	try {
		const auto target = oraduck::ParseTargetName(input.inputs[0].ToString());
		oraduck::OciSession session(credentials);
		oraduck::PrepareReadSession(session);
		source = oraduck::DescribeSource(session, target);
	} catch (const oraduck::OraduckError &e) {
		throw BinderException("OraDuck: " + string(e.what()));
	}
	return BindOraduckScan(context, std::move(credentials), std::move(source), options, return_types, names);
}

unique_ptr<GlobalTableFunctionState> OraduckScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<OraduckScanBindData>();
	auto state = make_uniq<OraduckScanGlobalState>();

	// Projection: column ids beyond the Oracle columns (the empty column of count(*)) fetch nothing
	vector<optional_idx> projected_index(bind.columns.size());
	for (auto &index : input.column_indexes) {
		const auto id = index.GetPrimaryIndex();
		if (id >= bind.columns.size()) {
			state->outputs.emplace_back();
			continue;
		}
		const auto &plan = bind.columns[id];
		if (plan.decoder == oraduck::FetchDecoder::kUnsupported) {
			throw BinderException("OraDuck: " + plan.unsupported);
		}
		if (!projected_index[id].IsValid()) {
			projected_index[id] = state->projected.size();
			state->projected.push_back(&plan);
		}
		state->outputs.push_back(projected_index[id]);
	}

	int64_t sessions = bind.options.sessions;
	if (sessions == 0) {
		sessions = MinValue<int64_t>(MaxValue<int64_t>(TaskScheduler::GetScheduler(context).NumberOfThreads(), 1),
		                             kMaxSessions);
	}
	try {
		auto session = std::make_unique<oraduck::OciSession>(bind.credentials);
		oraduck::PrepareReadSession(*session);
		const auto start = std::chrono::steady_clock::now();
		state->plan = oraduck::PlanWork(*session, bind.source, bind.options.split, bind.options.snapshot,
		                                static_cast<uint32_t>(sessions));
		state->planning_ms =
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		if (!state->plan.slices.empty()) {
			state->spare_session = std::move(session);
		}
	} catch (const oraduck::OraduckError &e) {
		throw IOException("OraDuck: " + string(e.what()));
	}
	state->sessions = MinValue<idx_t>(idx_t(sessions), state->plan.slices.size());

	state->sql.owner = bind.source.owner;
	state->sql.object = bind.source.name;
	state->sql.columns = state->projected;
	state->sql.filters = bind.filters;
	state->sql.snapshot_scn = state->plan.scn;
	const auto &slices = state->plan.slices;
	state->first_sql = InlineBinds(oraduck::BuildScanSql(state->sql, slices.empty() ? oraduck::Slice() : slices[0]));

	if (bind.options.snapshot == SnapshotMode::kAuto && state->plan.scn.empty()) {
		DUCKDB_LOG_WARNING(context, "OraDuck: oraduck_scan of " + DisplayName(bind.source) +
		                                " reads without a common snapshot: " + SnapshotNotes(state->plan.notes));
	}
	return std::move(state);
}

unique_ptr<LocalTableFunctionState> OraduckScanInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                         GlobalTableFunctionState *) {
	return make_uniq<OraduckScanLocalState>();
}

// The planning session if no thread took it yet, else a new session (logged in outside the lock, so that the
// threads log in at the same time)
std::unique_ptr<oraduck::OciSession> TakeSession(const OraduckScanBindData &bind, OraduckScanGlobalState &gstate) {
	{
		std::lock_guard<std::mutex> guard(gstate.lock);
		if (gstate.spare_session) {
			return std::move(gstate.spare_session);
		}
	}
	auto session = std::make_unique<oraduck::OciSession>(bind.credentials);
	oraduck::PrepareReadSession(*session);
	return session;
}

// Loads the next batch of rows: the current unit's next fetch, else the first of the next unit; false when no unit
// is left for this thread
bool NextBatch(ClientContext &context, const OraduckScanBindData &bind, OraduckScanGlobalState &gstate,
               OraduckScanLocalState &lstate) {
	while (true) {
		// An interrupt or max_execution_time stops the scan between two fetches
		context.BlockingInterruptCheck();
		if (lstate.cursor) {
			const uint32_t rows = lstate.cursor->Fetch();
			if (rows > 0) {
				lstate.batch_rows = rows;
				lstate.batch_next = 0;
				return true;
			}
			lstate.cursor.reset();
		}
		const idx_t unit = gstate.next_unit.fetch_add(1);
		if (unit >= gstate.plan.slices.size()) {
			lstate.session.reset(); // nothing left for this thread: release its session now
			lstate.finished = true;
			return false;
		}
		if (!lstate.session) {
			lstate.session = TakeSession(bind, gstate);
		}
		lstate.cursor = std::make_unique<oraduck::FetchCursor>(
		    *lstate.session, oraduck::BuildScanSql(gstate.sql, gstate.plan.slices[unit]), gstate.projected,
		    oraduck::FetchOptions());
	}
}

void OraduckScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<OraduckScanBindData>();
	auto &gstate = data.global_state->Cast<OraduckScanGlobalState>();
	auto &lstate = data.local_state->Cast<OraduckScanLocalState>();
	idx_t filled = 0;
	try {
		while (filled < STANDARD_VECTOR_SIZE && !lstate.finished) {
			if (lstate.batch_next == lstate.batch_rows) {
				if (!NextBatch(context, bind, gstate, lstate)) {
					break;
				}
				continue;
			}
			const auto count =
			    MinValue<uint32_t>(uint32_t(STANDARD_VECTOR_SIZE - filled), lstate.batch_rows - lstate.batch_next);
			for (idx_t i = 0; i < gstate.outputs.size(); i++) {
				if (gstate.outputs[i].IsValid()) {
					const auto column = gstate.outputs[i].GetIndex();
					WriteColumn(*lstate.cursor, column, *gstate.projected[column], lstate.batch_next, count,
					            output.data[i], filled);
				}
			}
			lstate.batch_next += count;
			filled += count;
		}
	} catch (const oraduck::OraduckError &e) {
		throw IOException("OraDuck: " + string(e.what()));
	}
	for (idx_t i = 0; i < gstate.outputs.size(); i++) {
		if (!gstate.outputs[i].IsValid()) {
			ConstantVector::SetNull(output.data[i], count_t(filled));
		}
	}
	output.SetChildCardinality(filled);
}

unique_ptr<NodeStatistics> OraduckScanCardinality(ClientContext &, const FunctionData *bind_data) {
	auto &bind = bind_data->Cast<OraduckScanBindData>();
	if (bind.source.num_rows < 0) {
		return nullptr;
	}
	// An estimate only: Oracle statistics can be stale, so no maximum
	return make_uniq<NodeStatistics>(idx_t(bind.source.num_rows));
}

virtual_column_map_t OraduckScanVirtualColumns(ClientContext &, optional_ptr<FunctionData>) {
	virtual_column_map_t result;
	// count(*) projects the empty column instead of an Oracle column
	result.insert(make_pair(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN)));
	return result;
}

InsertionOrderPreservingMap<string> OraduckScanToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	if (!input.bind_data) {
		return result;
	}
	auto &bind = input.bind_data->Cast<OraduckScanBindData>();
	result["Table"] = DisplayName(bind.source);
	string where;
	for (auto &filter : bind.filters) {
		where += (where.empty() ? "" : " AND ") + filter.display;
	}
	result["Oracle WHERE"] = where.empty() ? kNothing : where;
	result["Split"] = SplitText(bind.options.split);
	result["Snapshot"] = SnapshotText(bind.options.snapshot);
	return result;
}

// EXPLAIN ANALYZE: the work plan exists only once the query runs
void OraduckScanGetMetrics(TableFunctionGetMetricsInput &input) {
	if (!input.global_state) {
		return;
	}
	auto &gstate = input.global_state->Cast<OraduckScanGlobalState>();
	string columns;
	for (auto *column : gstate.projected) {
		columns += (columns.empty() ? "" : ", ") + column->name;
	}
	char planning[32];
	std::snprintf(planning, sizeof(planning), "%.1f", gstate.planning_ms);
	// A copy: the requested Snapshot of to_string is replaced by the snapshot used
	auto info = input.operator_metrics.GetExtraInfo();
	info["Columns"] = columns.empty() ? kNothing : columns;
	info["Strategy"] = SplitText(gstate.plan.strategy);
	info["Units"] = std::to_string(gstate.plan.slices.size());
	info["Sessions"] = std::to_string(gstate.sessions);
	info["Snapshot"] = gstate.plan.scn.empty() ? "per session" : gstate.plan.scn;
	info["Notes"] = gstate.plan.notes.empty() ? kNothing : gstate.plan.notes;
	info["Planning (ms)"] = planning;
	info["Oracle SQL"] = gstate.first_sql;
	input.operator_metrics.SetExtraInfo(std::move(info));
}

// The bind data holds the filters pushed to Oracle, whose DuckDB expressions are gone from the plan; a plan copy
// rebound from the call's arguments would lose them. NotImplementedException: what plan copies (CTE inlining, window
// self-join, partitioned execution) catch to give up copying.
void OraduckScanSerialize(Serializer &, const optional_ptr<FunctionData>, const BoundTableFunction &) {
	throw NotImplementedException("OraDuck: an oraduck_scan plan cannot be serialized");
}

unique_ptr<FunctionData> OraduckScanDeserialize(Deserializer &, BoundTableFunction &) {
	throw NotImplementedException("OraDuck: an oraduck_scan plan cannot be deserialized");
}

bool SameFilters(const std::vector<oraduck::SqlFragment> &a, const std::vector<oraduck::SqlFragment> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (idx_t i = 0; i < a.size(); i++) {
		if (a[i].sql != b[i].sql || a[i].display != b[i].display) {
			return false;
		}
	}
	return true;
}

} // namespace

unique_ptr<FunctionData> OraduckScanBindData::Copy() const {
	return make_uniq<OraduckScanBindData>(*this);
}

bool OraduckScanBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<OraduckScanBindData>();
	return credentials.dsn == other.credentials.dsn && credentials.user == other.credentials.user &&
	       source.owner == other.source.owner && source.name == other.source.name &&
	       options.sessions == other.options.sessions && options.split == other.options.split &&
	       options.snapshot == other.options.snapshot &&
	       options.unconstrained.kind == other.options.unconstrained.kind &&
	       options.unconstrained.width == other.options.unconstrained.width &&
	       options.unconstrained.scale == other.options.unconstrained.scale && SameFilters(filters, other.filters);
}

void ApplyScanOption(ScanOptions &options, const string &name, const Value &value) {
	const auto key = StringUtil::Lower(name);
	if (key == "sessions") {
		auto number = value.IsNull() ? optional<Value>() : value.DefaultTryCastAs(LogicalType::BIGINT);
		const auto sessions = number ? number->GetValue<int64_t>() : int64_t(0);
		if (sessions < 1 || sessions > kMaxSessions) {
			throw InvalidInputException("OraDuck: invalid value for sessions: %s (accepted: an integer from 1 to %d)",
			                            value.ToString(), kMaxSessions);
		}
		options.sessions = static_cast<uint32_t>(sessions);
		return;
	}
	try {
		if (key == "split") {
			options.split = oraduck::ParseSplitStrategy(value.ToString());
		} else if (key == "unconstrained_number") {
			options.unconstrained = oraduck::ParseUnconstrainedNumber(value.ToString());
		} else if (key == "snapshot") {
			options.snapshot = oraduck::ParseSnapshotMode(value.ToString());
		} else {
			throw InvalidInputException("OraDuck: unknown option \"%s\" (accepted: sessions, split, "
			                            "unconstrained_number, snapshot)",
			                            name);
		}
	} catch (const oraduck::OraduckError &e) {
		throw InvalidInputException("OraDuck: " + string(e.what()));
	}
}

vector<string> DuckColumnNames(const oraduck::OracleTableDescription &source) {
	// Lower-cased names that a generated name must avoid: every Oracle name and every name given so far
	std::unordered_set<string> taken;
	for (auto &column : source.columns) {
		taken.insert(StringUtil::Lower(column.name));
	}
	std::unordered_set<string> given;
	vector<string> names;
	names.reserve(source.columns.size());
	for (auto &column : source.columns) {
		if (given.insert(StringUtil::Lower(column.name)).second) {
			names.push_back(column.name);
			continue;
		}
		for (idx_t suffix = 1;; suffix++) {
			auto candidate = column.name + "_" + std::to_string(suffix);
			auto lower = StringUtil::Lower(candidate);
			if (!taken.count(lower) && !given.count(lower)) {
				taken.insert(lower);
				given.insert(lower);
				names.push_back(std::move(candidate));
				break;
			}
		}
	}
	return names;
}

unique_ptr<FunctionData> BindOraduckScan(ClientContext &, oraduck::OracleCredentials credentials,
                                         oraduck::OracleTableDescription source, const ScanOptions &options,
                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (source.columns.empty()) {
		throw BinderException("OraDuck: Oracle %s %s has no column visible to the user",
		                      source.kind == oraduck::SourceKind::kView ? "view" : "table", DisplayName(source));
	}
	auto result = make_uniq<OraduckScanBindData>();
	result->credentials = std::move(credentials);
	result->options = options;
	result->columns.reserve(source.columns.size());
	for (auto &column : source.columns) {
		result->columns.push_back(oraduck::PlanReadColumn(column, options.unconstrained, source.db_charset));
		return_types.push_back(ToLogicalType(result->columns.back().duck_type));
	}
	for (auto &name : DuckColumnNames(source)) {
		names.emplace_back(name);
	}
	result->source = std::move(source);
	return std::move(result);
}

TableFunction GetOraduckScanFunction() {
	TableFunction function("oraduck_scan", {LogicalType::VARCHAR}, OraduckScan, OraduckScanBind, OraduckScanInitGlobal,
	                       OraduckScanInitLocal);
	function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("connection", LogicalType::VARCHAR)
		    .Add("sessions", LogicalType::INTEGER)
		    .Add("split", LogicalType::VARCHAR)
		    .Add("unconstrained_number", LogicalType::VARCHAR)
		    .Add("snapshot", LogicalType::VARCHAR);
	});
	function.projection_pushdown = true;
	// Filters that translate into Oracle SQL go to the Oracle WHERE (partition pruning), the others stay in DuckDB
	function.pushdown_complex_filter = OraduckScanPushdownComplexFilter;
	// No plan serialization (the pushed filters live only in the bind data): the common subplan optimizer skips the
	// scan, plan copies fail and the optimizers that need them give up
	function.verify_serialization = false;
	function.SetSerializeCallback(OraduckScanSerialize);
	function.SetDeserializeCallback(OraduckScanDeserialize);
	function.filter_pushdown = false;
	function.filter_prune = false;
	// Units are read in parallel: the scan does not keep Oracle's row order
	function.order_preservation_type = OrderPreservationType::NO_ORDER;
	function.cardinality = OraduckScanCardinality;
	function.get_virtual_columns = OraduckScanVirtualColumns;
	function.to_string = OraduckScanToString;
	function.get_metrics = OraduckScanGetMetrics;
	return function;
}

void RegisterOraduckScan(ExtensionLoader &loader) {
	loader.RegisterFunction(GetOraduckScanFunction());
}

} // namespace duckdb

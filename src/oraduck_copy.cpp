#include "oraduck_copy.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/persistent/physical_copy_to_file.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

// OraDuck (OCI) headers after DuckDB
#include "oraduck_secret.hpp"
#include "oraduck/chunk_writer.hpp"
#include "oraduck/column_plan.hpp"
#include "oraduck/describe_target.hpp"

#include <memory>
#include <mutex>
#include <thread>

namespace duckdb {

namespace {

constexpr uint64_t kMinStreamSize = 64 * 1024;
constexpr uint64_t kMaxStreamSize = 256 * 1024 * 1024;

struct OraduckBindData : public FunctionData {
	oraduck::OracleCredentials credentials;
	oraduck::TableDescription table;
	std::vector<oraduck::PlannedColumn> plan;
	oraduck::LoaderOptions loader_options;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<OraduckBindData>(*this);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<OraduckBindData>();
		return credentials.dsn == other.credentials.dsn && credentials.user == other.credentials.user &&
		       table.owner == other.table.owner && table.table == other.table.table &&
		       loader_options.stream_buffer_bytes == other.loader_options.stream_buffer_bytes &&
		       loader_options.skip_index_maintenance == other.loader_options.skip_index_maintenance;
	}
};

// One Oracle session and its work buffers, used by one thread at a time
struct OraduckWriter {
	std::unique_ptr<oraduck::DirectPathLoader> loader;
	std::vector<std::vector<uint8_t>> scratch;
	std::vector<UnifiedVectorFormat> formats;
};

// DuckDB hands the rows over in batches, from several threads at once: each batch borrows an idle session, so
// there are at most as many sessions as threads loading at the same time
struct OraduckGlobalState : public GlobalFunctionData {
	std::mutex lock;
	// Every session of the load, committed together in Finalize
	std::vector<std::unique_ptr<OraduckWriter>> writers;
	std::vector<OraduckWriter *> idle;
};

void OraduckCopyOptions(ClientContext &, CopyOptionsInput &input) {
	input.options["connection"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	input.options["stream_size"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["skip_index_maintenance"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
}

unique_ptr<FunctionData> OraduckBind(ClientContext &context, CopyFunctionBindInput &input,
                                      const vector<Identifier> &names, const vector<LogicalType> &sql_types) {
	auto &info = input.info;
	auto result = make_uniq<OraduckBindData>();
	string secret_name;
	for (auto &option : info.options) {
		const auto &key = option.first; // Identifier: compared case-insensitively
		if (key == "connection") {
			secret_name = option.second[0].ToString();
		} else if (key == "stream_size") {
			const auto size = option.second[0].GetValue<uint64_t>();
			if (size < kMinStreamSize || size > kMaxStreamSize) {
				throw BinderException("OraDuck: STREAM_SIZE must be between 65536 and 268435456 bytes");
			}
			result->loader_options.stream_buffer_bytes = static_cast<oraduck::ub4>(size);
		} else if (key == "skip_index_maintenance") {
			result->loader_options.skip_index_maintenance = option.second.empty() || BooleanValue::Get(
			                                                    option.second[0].DefaultCastAs(LogicalType::BOOLEAN));
		}
	}
	if (secret_name.empty()) {
		throw BinderException("OraDuck: the CONNECTION option is required (name of an oraduck secret)");
	}
	if (FileSystem::GetFileSystem(context).FileExists(info.file_path)) {
		throw BinderException("OraDuck: a local file named \"" + info.file_path +
		                      "\" exists in the current directory; DuckDB would replace it. Rename it or run DuckDB "
		                      "from another directory.");
	}
	result->credentials = GetOraduckCredentials(context, secret_name);
	try {
		const auto target = oraduck::ParseTargetName(info.file_path);
		oraduck::OciSession session(result->credentials);
		result->table = oraduck::DescribeTarget(session, target);
	} catch (const oraduck::OraduckError &e) {
		throw BinderException("OraDuck: " + string(e.what()));
	}
	if (result->table.index_count > 0 && !result->loader_options.skip_index_maintenance) {
		throw BinderException("OraDuck: table " + result->table.owner + "." + result->table.table + " has " +
		                      std::to_string(result->table.index_count) +
		                      " index(es); parallel direct path loads do not maintain indexes (ORA-26002). "
		                      "Use SKIP_INDEX_MAINTENANCE true to load it and leave its indexes UNUSABLE (rebuild "
		                      "them afterwards with ALTER INDEX ... REBUILD), or drop the indexes.");
	}
	// Column names as written in the query: the plan matches them exactly first, then case-insensitively
	std::vector<std::string> column_names;
	column_names.reserve(names.size());
	for (const auto &name : names) {
		column_names.push_back(name.GetIdentifierName());
	}
	result->plan = oraduck::BuildColumnPlan(column_names, sql_types, result->table);
	return std::move(result);
}

unique_ptr<GlobalFunctionData> OraduckInitGlobal(ClientContext &, FunctionData &, const string &) {
	return make_uniq<OraduckGlobalState>();
}

void OraduckInitializeOperator(GlobalFunctionData &, const PhysicalOperator &op) {
	auto &copy = op.Cast<PhysicalCopyToFile>();
	if (copy.per_thread_output || copy.partition_output || copy.batches_per_file.IsValid() ||
	    copy.file_size_bytes.IsValid() || copy.use_tmp_file) {
		throw NotImplementedException("OraDuck: file options (PER_THREAD_OUTPUT, PARTITION_BY, "
		                              "FILE_SIZE_BYTES, USE_TMP_FILE) do not apply to FORMAT ORADUCK");
	}
}

std::unique_ptr<OraduckWriter> OpenWriter(const OraduckBindData &bind) {
	std::vector<oraduck::DirPathColumnSpec> specs;
	specs.reserve(bind.plan.size());
	for (const auto &column : bind.plan) {
		specs.push_back(column.spec);
	}
	auto writer = std::make_unique<OraduckWriter>();
	writer->loader = std::make_unique<oraduck::DirectPathLoader>(bind.credentials, bind.table.owner, bind.table.table,
	                                                              specs, bind.loader_options);
	writer->scratch.resize(bind.plan.size());
	for (idx_t i = 0; i < bind.plan.size(); i++) {
		writer->scratch[i].resize(size_t(writer->loader->Rows()) * bind.plan[i].scratch_width);
	}
	writer->formats.resize(bind.plan.size());
	return writer;
}

OraduckWriter &AcquireWriter(const OraduckBindData &bind, OraduckGlobalState &gstate) {
	{
		std::lock_guard<std::mutex> guard(gstate.lock);
		if (!gstate.idle.empty()) {
			auto writer = gstate.idle.back();
			gstate.idle.pop_back();
			return *writer;
		}
	}
	auto writer = OpenWriter(bind); // logs in: outside the lock
	std::lock_guard<std::mutex> guard(gstate.lock);
	gstate.writers.push_back(std::move(writer));
	return *gstate.writers.back();
}

void ReleaseWriter(OraduckGlobalState &gstate, OraduckWriter &writer) {
	std::lock_guard<std::mutex> guard(gstate.lock);
	gstate.idle.push_back(&writer);
}

void WriteChunk(const OraduckBindData &bind, OraduckWriter &writer, DataChunk &input) {
	const idx_t count = input.size();
	auto &loader = *writer.loader;
	for (idx_t i = 0; i < bind.plan.size(); i++) {
		input.data[bind.plan[i].source_index].ToUnifiedFormat(writer.formats[i]);
	}
	const idx_t capacity = loader.Rows();
	for (idx_t offset = 0; offset < count; offset += capacity) {
		const idx_t n = MinValue<idx_t>(capacity, count - offset);
		for (idx_t i = 0; i < bind.plan.size(); i++) {
			oraduck::WriteColumn(bind.plan[i], static_cast<oraduck::ub2>(i), writer.formats[i], offset, n,
			                      writer.scratch[i].data(), loader);
		}
		loader.ConvertAndLoad(static_cast<oraduck::ub4>(n));
	}
}

// Sends the batch to Oracle; runs on several threads at once (the flush that follows is serialized by DuckDB)
unique_ptr<PreparedBatchData> OraduckPrepareBatch(ClientContext &, FunctionData &bind_p, GlobalFunctionData &gstate_p,
                                                  unique_ptr<ColumnDataCollection> collection) {
	auto &bind = bind_p.Cast<OraduckBindData>();
	auto &gstate = gstate_p.Cast<OraduckGlobalState>();
	try {
		// On error the writer is not released: the query fails and its session is aborted
		auto &writer = AcquireWriter(bind, gstate);
		for (auto &chunk : collection->Chunks()) {
			WriteChunk(bind, writer, chunk);
		}
		ReleaseWriter(gstate, writer);
	} catch (const oraduck::OraduckError &e) {
		throw IOException("OraDuck: " + string(e.what()));
	}
	return make_uniq<PreparedBatchData>();
}

void OraduckFlushBatch(ClientContext &, FunctionData &, GlobalFunctionData &, PreparedBatchData &) {
	// the rows were sent by OraduckPrepareBatch
}

void OraduckFinalize(ClientContext &, FunctionData &, GlobalFunctionData &gstate_p) {
	auto &gstate = gstate_p.Cast<OraduckGlobalState>();
	auto &writers = gstate.writers;
	std::vector<std::string> errors(writers.size());
	std::vector<std::thread> threads;
	threads.reserve(writers.size());
	for (size_t i = 0; i < writers.size(); i++) {
		threads.emplace_back([&writers, &errors, i]() {
			try {
				writers[i]->loader->Finish();
			} catch (const std::exception &e) {
				errors[i] = e.what();
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	idx_t failed = 0;
	string first_error;
	for (const auto &error : errors) {
		if (!error.empty() && failed++ == 0) {
			first_error = error;
		}
	}
	const idx_t sessions = errors.size();
	gstate.idle.clear();
	writers.clear(); // aborts the failed sessions
	if (failed == 0) {
		return;
	}
	if (failed == sessions) {
		throw IOException("OraDuck: commit failed, no data loaded: " + first_error);
	}
	throw IOException("OraDuck: PARTIAL load: " + std::to_string(sessions - failed) +
	                  " session(s) committed, " + std::to_string(failed) + " failed: " + first_error);
}

CopyFunctionExecutionMode OraduckExecutionMode(bool, bool) {
	return CopyFunctionExecutionMode::PARALLEL_COPY_TO_FILE;
}

} // namespace

void RegisterOraduckCopy(ExtensionLoader &loader) {
	CopyFunction function("oraduck");
	function.copy_options = OraduckCopyOptions;
	function.copy_to_bind = OraduckBind;
	function.copy_to_initialize_global = OraduckInitGlobal;
	function.initialize_operator = OraduckInitializeOperator;
	function.prepare_batch = OraduckPrepareBatch;
	function.flush_batch = OraduckFlushBatch;
	function.copy_to_finalize = OraduckFinalize;
	function.execution_mode = OraduckExecutionMode;
	loader.RegisterFunction(function);
}

} // namespace duckdb

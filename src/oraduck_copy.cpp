#include "oraduck_copy.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
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

struct OraduckGlobalState : public GlobalFunctionData {
	std::mutex lock;
	// Sessions whose data has been sent, committed together in Finalize
	std::vector<std::unique_ptr<oraduck::DirectPathLoader>> loaders;
};

struct OraduckLocalState : public LocalFunctionData {
	std::unique_ptr<oraduck::DirectPathLoader> loader; // opened on the first Sink
	std::vector<std::vector<uint8_t>> scratch;
	std::vector<UnifiedVectorFormat> formats;
};

void OraduckCopyOptions(ClientContext &, CopyOptionsInput &input) {
	input.options["connection"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	input.options["stream_size"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["skip_index_maintenance"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
}

unique_ptr<FunctionData> OraduckBind(ClientContext &context, CopyFunctionBindInput &input,
                                      const vector<string> &names, const vector<LogicalType> &sql_types) {
	auto &info = input.info;
	auto result = make_uniq<OraduckBindData>();
	string secret_name;
	for (auto &option : info.options) {
		const auto key = StringUtil::Lower(option.first);
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
	result->plan = oraduck::BuildColumnPlan(names, sql_types, result->table);
	return std::move(result);
}

unique_ptr<GlobalFunctionData> OraduckInitGlobal(ClientContext &, FunctionData &, const string &) {
	return make_uniq<OraduckGlobalState>();
}

unique_ptr<LocalFunctionData> OraduckInitLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<OraduckLocalState>();
}

void OraduckInitializeOperator(GlobalFunctionData &, const PhysicalOperator &op) {
	auto &copy = op.Cast<PhysicalCopyToFile>();
	if (copy.per_thread_output || copy.partition_output || copy.rotate || copy.file_size_bytes.IsValid() ||
	    copy.use_tmp_file) {
		throw NotImplementedException("OraDuck: file options (PER_THREAD_OUTPUT, PARTITION_BY, "
		                              "FILE_SIZE_BYTES, USE_TMP_FILE) do not apply to FORMAT ORADUCK");
	}
}

void OpenLoader(const OraduckBindData &bind, OraduckLocalState &state) {
	std::vector<oraduck::DirPathColumnSpec> specs;
	specs.reserve(bind.plan.size());
	for (const auto &column : bind.plan) {
		specs.push_back(column.spec);
	}
	state.loader = std::make_unique<oraduck::DirectPathLoader>(bind.credentials, bind.table.owner, bind.table.table,
	                                                            specs, bind.loader_options);
	state.scratch.resize(bind.plan.size());
	for (idx_t i = 0; i < bind.plan.size(); i++) {
		state.scratch[i].resize(size_t(state.loader->Rows()) * bind.plan[i].scratch_width);
	}
	state.formats.resize(bind.plan.size());
}

void OraduckSink(ExecutionContext &, FunctionData &bind_p, GlobalFunctionData &, LocalFunctionData &lstate_p,
                  DataChunk &input) {
	auto &bind = bind_p.Cast<OraduckBindData>();
	auto &state = lstate_p.Cast<OraduckLocalState>();
	const idx_t count = input.size();
	if (count == 0) {
		return;
	}
	try {
		if (!state.loader) {
			OpenLoader(bind, state);
		}
		auto &loader = *state.loader;
		for (idx_t i = 0; i < bind.plan.size(); i++) {
			input.data[bind.plan[i].source_index].ToUnifiedFormat(count, state.formats[i]);
		}
		const idx_t capacity = loader.Rows();
		for (idx_t offset = 0; offset < count; offset += capacity) {
			const idx_t n = MinValue<idx_t>(capacity, count - offset);
			for (idx_t i = 0; i < bind.plan.size(); i++) {
				oraduck::WriteColumn(bind.plan[i], static_cast<oraduck::ub2>(i), state.formats[i], offset, n,
				                      state.scratch[i].data(), loader);
			}
			loader.ConvertAndLoad(static_cast<oraduck::ub4>(n));
		}
	} catch (const oraduck::OraduckError &e) {
		throw IOException("OraDuck: " + string(e.what()));
	}
}

void OraduckCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &gstate_p, LocalFunctionData &lstate_p) {
	auto &gstate = gstate_p.Cast<OraduckGlobalState>();
	auto &state = lstate_p.Cast<OraduckLocalState>();
	if (!state.loader) {
		return;
	}
	try {
		state.loader->Flush();
	} catch (const oraduck::OraduckError &e) {
		throw IOException("OraDuck: " + string(e.what()));
	}
	std::lock_guard<std::mutex> guard(gstate.lock);
	gstate.loaders.push_back(std::move(state.loader));
}

void OraduckFinalize(ClientContext &, FunctionData &, GlobalFunctionData &gstate_p) {
	auto &gstate = gstate_p.Cast<OraduckGlobalState>();
	auto &loaders = gstate.loaders;
	std::vector<std::string> errors(loaders.size());
	std::vector<std::thread> threads;
	threads.reserve(loaders.size());
	for (size_t i = 0; i < loaders.size(); i++) {
		threads.emplace_back([&loaders, &errors, i]() {
			try {
				loaders[i]->Finish();
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
	loaders.clear(); // aborts the failed sessions
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
	function.copy_to_initialize_local = OraduckInitLocal;
	function.initialize_operator = OraduckInitializeOperator;
	function.copy_to_sink = OraduckSink;
	function.copy_to_combine = OraduckCombine;
	function.copy_to_finalize = OraduckFinalize;
	function.execution_mode = OraduckExecutionMode;
	loader.RegisterFunction(function);
}

} // namespace duckdb

PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Extension configuration
EXT_NAME=oraduck
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# DuckDB 2 no longer reads its version from git: take it from the submodule's tag (the CI sets it explicitly)
OVERRIDE_GIT_DESCRIBE ?= $(shell git -C $(PROJ_DIR)duckdb describe --tags --exact-match 2>/dev/null)

# Shared DuckDB extension Makefile (release, debug, test, ...)
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# --- OraDuck targets outside the DuckDB build: C++ tests and probes (itest and probes: source env.sh) ---
.PHONY: unit itest probe read-probe decode-bench

ORADUCK_CXX ?= g++
ORADUCK_CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -I$(PROJ_DIR)src/include
CATCH_FLAGS := -I$(PROJ_DIR)duckdb/third_party/catch
# OCI is loaded at run time (src/oci_api.cpp, in PURE_SRCS): Instant Client in LD_LIBRARY_PATH
OCI_LIBS = -ldl -pthread
ORADUCK_HEADERS = $(wildcard src/include/oraduck/*.hpp)
PURE_SRCS := src/oracle_encoding.cpp src/oracle_decoding.cpp src/oracle_names.cpp src/oci_api.cpp src/read_types.cpp src/sql_binds.cpp src/filter_sql.cpp src/read_sql.cpp src/rowid_ranges.cpp
OCI_SRCS = $(wildcard src/oci.cpp src/oci_session.cpp src/describe_target.cpp src/describe_source.cpp src/direct_path_loader.cpp)
UNIT_SRCS = test/cpp/catch_main.cpp $(wildcard test/cpp/test_*.cpp)
ITEST_SRCS = test/cpp/catch_main.cpp $(wildcard test/cpp/itest_*.cpp)

build/unit/unit_tests: $(UNIT_SRCS) $(PURE_SRCS) $(ORADUCK_HEADERS)
	@mkdir -p $(dir $@)
	$(ORADUCK_CXX) $(ORADUCK_CXXFLAGS) $(CATCH_FLAGS) $(UNIT_SRCS) $(PURE_SRCS) -ldl -o $@

unit: build/unit/unit_tests
	./build/unit/unit_tests

build/itest/itests: $(ITEST_SRCS) $(PURE_SRCS) $(OCI_SRCS) $(ORADUCK_HEADERS) $(wildcard test/cpp/*.hpp)
	@mkdir -p $(dir $@)
	$(ORADUCK_CXX) $(ORADUCK_CXXFLAGS) $(CATCH_FLAGS) $(ITEST_SRCS) $(PURE_SRCS) $(OCI_SRCS) $(OCI_LIBS) -o $@

itest: build/itest/itests
	./build/itest/itests

build/probe/oraduck_probe: tools/probe.cpp $(PURE_SRCS) $(OCI_SRCS) $(ORADUCK_HEADERS)
	@mkdir -p $(dir $@)
	$(ORADUCK_CXX) $(ORADUCK_CXXFLAGS) tools/probe.cpp $(PURE_SRCS) $(OCI_SRCS) $(OCI_LIBS) -o $@

probe: build/probe/oraduck_probe
	./build/probe/oraduck_probe

build/probe/oraduck_read_probe: tools/read_probe.cpp $(PURE_SRCS) $(OCI_SRCS) $(ORADUCK_HEADERS)
	@mkdir -p $(dir $@)
	$(ORADUCK_CXX) $(ORADUCK_CXXFLAGS) tools/read_probe.cpp $(PURE_SRCS) $(OCI_SRCS) $(OCI_LIBS) -o $@

read-probe: build/probe/oraduck_read_probe
	./build/probe/oraduck_read_probe $(READ_PROBE_ARGS)

build/probe/oraduck_decode_bench: tools/decode_bench.cpp $(PURE_SRCS) $(ORADUCK_HEADERS)
	@mkdir -p $(dir $@)
	$(ORADUCK_CXX) $(ORADUCK_CXXFLAGS) tools/decode_bench.cpp $(PURE_SRCS) -ldl -o $@

decode-bench: build/probe/oraduck_decode_bench
	taskset -c 0-7 ./build/probe/oraduck_decode_bench

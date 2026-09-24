PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Extension configuration
EXT_NAME=oraduck
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Shared DuckDB extension Makefile (release, debug, test, ...)
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# --- OraDuck targets outside the DuckDB build: C++ tests and probe (itest and probe: source env.sh) ---
.PHONY: unit itest probe

ORADUCK_CXX ?= g++
ORADUCK_CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -I$(PROJ_DIR)src/include
CATCH_FLAGS := -I$(PROJ_DIR)duckdb/third_party/catch
# OCI is loaded at run time (src/oci_api.cpp, in PURE_SRCS): Instant Client in LD_LIBRARY_PATH
OCI_LIBS = -ldl -pthread
ORADUCK_HEADERS = $(wildcard src/include/oraduck/*.hpp)
PURE_SRCS := src/oracle_encoding.cpp src/oracle_names.cpp src/oci_api.cpp
OCI_SRCS = $(wildcard src/oci.cpp src/oci_session.cpp src/describe_target.cpp src/direct_path_loader.cpp)
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

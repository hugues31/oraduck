cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME oraduck)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{OCI_HOME})
  message(FATAL_ERROR "OCI_HOME is not set: run 'source env.sh' (Instant Client 19 with its SDK)")
endif()
file(TO_CMAKE_PATH "$ENV{OCI_HOME}" OCI_HOME)
if(WIN32)
  set(OCI_LIBRARY ${OCI_HOME}/sdk/lib/msvc/oci.lib)
else()
  set(OCI_LIBRARY ${OCI_HOME}/libclntsh.so)
endif()

include_directories(src/include ${OCI_HOME}/sdk/include)

set(EXTENSION_SOURCES
    src/oraduck_extension.cpp
    src/oraduck_secret.cpp
    src/oracle_encoding.cpp
    src/oracle_names.cpp
    src/oci.cpp
    src/oci_session.cpp
    src/describe_target.cpp
    src/direct_path_loader.cpp
    src/oraduck_copy.cpp
    src/column_plan.cpp
    src/chunk_writer.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# DuckDB forces C++11 through the CMake cache; the extension is written in C++17
set_target_properties(${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

if(WIN32)
  # oci.h includes windows.h
  target_compile_definitions(${EXTENSION_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_link_libraries(${EXTENSION_NAME} ${OCI_LIBRARY})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${OCI_LIBRARY})
# Instant Client is found at load time (LD_LIBRARY_PATH or PATH): no build path in the shipped extension
set_target_properties(${LOADABLE_EXTENSION_NAME} PROPERTIES SKIP_BUILD_RPATH ON)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME oraduck)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OCI is loaded at run time from Oracle Instant Client (src/oci_api.cpp): no Oracle SDK or library
# is needed to build
include_directories(src/include)

set(EXTENSION_SOURCES
    src/oraduck_extension.cpp
    src/oraduck_secret.cpp
    src/oracle_encoding.cpp
    src/oracle_names.cpp
    src/oci_api.cpp
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
  # oci_api.cpp includes windows.h
  target_compile_definitions(${EXTENSION_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_link_libraries(${EXTENSION_NAME} ${CMAKE_DL_LIBS})
  target_link_libraries(${LOADABLE_EXTENSION_NAME} ${CMAKE_DL_LIBS})
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

# Included by the DuckDB build: extensions to load
duckdb_extension_load(oraduck
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)
# Built into build/release/duckdb (end-to-end tests, benchmark): DuckDB 2 only builds the loadable extension otherwise
duckdb_extension_statically_link(oraduck)

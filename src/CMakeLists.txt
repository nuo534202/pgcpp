# src/CMakeLists.txt - MyToyDB source library
#
# Builds the mytoydb_core static library from all module sources. New source
# files are added here as modules are converted from PostgreSQL C to C++20.

# ---------------------------------------------------------------------------
# Flex / Bison — generate scanner and parser for the SQL parser (M5)
# ---------------------------------------------------------------------------
find_package(BISON 3.0 REQUIRED)
find_package(FLEX 2.6 REQUIRED)

# Bison generates parser.cpp and parser.hpp from gram.yy
# Use C++ skeleton (lalr1.cc) for clean C++ integration.
BISON_TARGET(
    mytoydb_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/gram.yy
    ${CMAKE_CURRENT_BINARY_DIR}/parser/gram.tab.cpp
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser/gram.tab.hpp
    COMPILE_FLAGS "-Wno-conflicts-sr -Wno-conflicts-rr"
)

# Flex generates scanner.cpp from scan.ll
# Use C++ mode for clean C++ integration.
FLEX_TARGET(
    mytoydb_scanner
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/scan.ll
    ${CMAKE_CURRENT_BINARY_DIR}/parser/lex.yy.cpp
    COMPILE_FLAGS "--c++"
)

set(MYTOYDB_CORE_SOURCES
    common/memory/memory_context.cpp
    common/memory/alloc_set.cpp
    common/error/elog.cpp
    common/containers/node.cpp
    common/containers/list.cpp
    common/containers/string_info.cpp
    types/builtins.cpp
    types/datetime.cpp
    types/string_funcs.cpp
    catalog/bootstrap_catalog.cpp
    catalog/catalog.cpp
    catalog/syscache.cpp
    parser/keywords.cpp
    parser/parser.cpp
    parser/scanner.cpp
    parser/parsenodes.cpp
    parser/primnodes.cpp
    parser/parse_node.cpp
    parser/parse_type.cpp
    parser/parse_coerce.cpp
    parser/parse_oper.cpp
    parser/parse_func.cpp
    parser/parse_relation.cpp
    parser/parse_expr.cpp
    parser/parse_target.cpp
    parser/parse_clause.cpp
    parser/parse_agg.cpp
    parser/analyze.cpp
    ${BISON_mytoydb_parser_OUTPUTS}
    ${FLEX_mytoydb_scanner_OUTPUTS}
    storage/smgr/smgr.cpp
    storage/smgr/md.cpp
    storage/buffer/bufpage.cpp
    storage/buffer/bufmgr.cpp
    storage/buffer/freelist.cpp
    transaction/transam.cpp
    transaction/xact.cpp
    transaction/snapshot.cpp
    transaction/visibility.cpp
    transaction/lock.cpp
    transaction/lmgr.cpp
    access/rel.cpp
    access/heap/heapam.cpp
    access/nbtree/nbtree.cpp
    access/nbtree/nbtpage.cpp
    executor/exec_expr.cpp
    executor/exec_main.cpp
    executor/exec_utils.cpp
    executor/estate.cpp
    executor/node_exec.cpp
    executor/tupletable.cpp
    executor/node_agg.cpp
    executor/node_hash.cpp
    executor/node_hashjoin.cpp
    executor/node_indexscan.cpp
    executor/node_modify_table.cpp
    executor/node_nestloop.cpp
    executor/node_seqscan.cpp
    executor/node_sort.cpp
    optimizer/plan/planner.cpp
    optimizer/plan/subplanner.cpp
    optimizer/path/allpaths.cpp
    optimizer/path/indxpath.cpp
    optimizer/cost/costsize.cpp
)

add_library(mytoydb_core STATIC ${MYTOYDB_CORE_SOURCES})

target_compile_features(mytoydb_core PUBLIC cxx_std_20)
target_include_directories(mytoydb_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/parser
)

# Apply warning flags from the root CMakeLists.txt
target_compile_options(mytoydb_core PRIVATE -Wall -Wextra -Wpedantic)

# Generated Flex/Bison code triggers various warnings; suppress them.
set_source_files_properties(
    ${BISON_mytoydb_parser_OUTPUTS}
    ${FLEX_mytoydb_scanner_OUTPUTS}
    PROPERTIES
        COMPILE_OPTIONS "-Wno-unused;-Wno-sign-compare;-Wno-old-style-cast"
)

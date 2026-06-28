# src/CMakeLists.txt - pgcpp source library
#
# Builds the pgcpp_core static library from all module sources. New source
# files are added here as modules are converted from PostgreSQL C to C++20.

# ---------------------------------------------------------------------------
# Flex / Bison — generate scanner and parser for the SQL parser (M5)
# ---------------------------------------------------------------------------
find_package(BISON 3.0 REQUIRED)
find_package(FLEX 2.6 REQUIRED)

# Bison generates parser.cpp and parser.hpp from gram.yy
# Use C++ skeleton (lalr1.cc) for clean C++ integration.
BISON_TARGET(
    pgcpp_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/gram.yy
    ${CMAKE_CURRENT_BINARY_DIR}/parser/gram.tab.cpp
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser/gram.tab.hpp
    COMPILE_FLAGS "-Wno-conflicts-sr -Wno-conflicts-rr"
)

# Flex generates scanner.cpp from scan.ll
# Use C++ mode for clean C++ integration.
FLEX_TARGET(
    pgcpp_scanner
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/scan.ll
    ${CMAKE_CURRENT_BINARY_DIR}/parser/lex.yy.cpp
    COMPILE_FLAGS "--c++"
)

set(PGCPP_CORE_SOURCES
    common/memory/memory_context.cpp
    common/memory/alloc_set.cpp
    common/error/elog.cpp
    common/containers/node.cpp
    common/containers/list.cpp
    common/containers/string_info.cpp
    common/containers/bitmapset.cpp
    common/containers/node_funcs.cpp
    common/containers/makefuncs.cpp
    types/builtins.cpp
    types/datetime.cpp
    types/numeric.cpp
    types/numutils.cpp
    types/string_funcs.cpp
    types/misc_types.cpp
    types/oid_types.cpp
    types/network_types.cpp
    types/uuid.cpp
    types/varbit.cpp
    types/array.cpp
    types/json.cpp
    types/range.cpp
    types/rowtypes.cpp
    types/geo.cpp
    types/enum_types.cpp
    types/acl.cpp
    types/windowfunctions.cpp
    types/orderedsetaggs.cpp
    types/ts_types.cpp
    types/xml.cpp
    types/selfuncs.cpp
    types/ruleutils.cpp
    catalog/bootstrap_catalog.cpp
    catalog/catalog.cpp
    catalog/syscache.cpp
    catalog/lsyscache.cpp
    catalog/namespace.cpp
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
    parser/parse_utilcmd.cpp
    parser/parse_cte.cpp
    parser/analyze.cpp
    ${BISON_pgcpp_parser_OUTPUTS}
    ${FLEX_pgcpp_scanner_OUTPUTS}
    storage/smgr/smgr.cpp
    storage/smgr/md.cpp
    storage/buffer/bufpage.cpp
    storage/buffer/bufmgr.cpp
    storage/buffer/freelist.cpp
    storage/ipc/latch.cpp
    storage/ipc/shmem.cpp
    storage/ipc/shmqueue.cpp
    storage/ipc/shm_mq.cpp
    storage/ipc/ipci.cpp
    storage/ipc/proc.cpp
    storage/ipc/lwlock.cpp
    storage/ipc/predicate.cpp
    storage/ipc/deadlock.cpp
    storage/file/fd.cpp
    storage/sync/sync.cpp
    storage/freespace/freespace.cpp
    storage/freespace/fsmpage.cpp
    storage/large_object/inv_api.cpp
    transaction/transam.cpp
    transaction/xact.cpp
    transaction/snapshot.cpp
    transaction/visibility.cpp
    transaction/lock.cpp
    transaction/lmgr.cpp
    transaction/xlog.cpp
    transaction/xloginsert.cpp
    transaction/xlogreader.cpp
    transaction/xlogrecovery.cpp
    transaction/slru.cpp
    transaction/commit_ts.cpp
    transaction/multixact.cpp
    transaction/procarray.cpp
    transaction/sinval.cpp
    transaction/parallel.cpp
    access/rel.cpp
    access/common/tupdesc.cpp
    access/common/indextuple.cpp
    access/heap/heapam.cpp
    access/index/indexam.cpp
    access/index/genam.cpp
    access/nbtree/nbtree.cpp
    access/nbtree/nbtpage.cpp
    executor/exec_expr.cpp
    executor/exec_main.cpp
    executor/exec_utils.cpp
    executor/estate.cpp
    executor/node_exec.cpp
    executor/tupletable.cpp
    executor/node_agg.cpp
    executor/node_append.cpp
    executor/node_ctescan.cpp
    executor/node_hash.cpp
    executor/node_hashjoin.cpp
    executor/node_indexscan.cpp
    executor/node_limit.cpp
    executor/node_material.cpp
    executor/node_mergejoin.cpp
    executor/node_modify_table.cpp
    executor/node_nestloop.cpp
    executor/node_seqscan.cpp
    executor/node_sort.cpp
    executor/node_subqueryscan.cpp
    executor/node_unique.cpp
    executor/node_windowagg.cpp
    optimizer/plan/planner.cpp
    optimizer/plan/subplanner.cpp
    optimizer/plan/create_plan.cpp
    optimizer/plan/init_splan.cpp
    optimizer/plan/set_refs.cpp
    optimizer/plan/subselect.cpp
    optimizer/path/allpaths.cpp
    optimizer/path/indxpath.cpp
    optimizer/path/equivclass.cpp
    optimizer/path/pathkeys.cpp
    optimizer/path/joinpath.cpp
    optimizer/path/joinrels.cpp
    optimizer/geqo/geqo_main.cpp
    optimizer/geqo/geqo_random.cpp
    optimizer/geqo/geqo_params.cpp
    optimizer/geqo/geqo_recombination.cpp
    optimizer/geqo/geqo_copy.cpp
    optimizer/geqo/geqo_mutation.cpp
    optimizer/geqo/geqo_cx.cpp
    optimizer/geqo/geqo_erx.cpp
    optimizer/geqo/geqo_pmx.cpp
    optimizer/geqo/geqo_ox1.cpp
    optimizer/geqo/geqo_ox2.cpp
    optimizer/geqo/geqo_px.cpp
    optimizer/geqo/geqo_eval.cpp
    optimizer/geqo/geqo_io.cpp
    optimizer/geqo/geqo_misc.cpp
    optimizer/cost/costsize.cpp
    optimizer/util/pathnode.cpp
    optimizer/util/relnode.cpp
    optimizer/util/restrictinfo.cpp
    optimizer/util/plancat.cpp
    protocol/pqformat.cpp
    protocol/dest.cpp
    protocol/postgres.cpp
    protocol/utility.cpp
    commands/tablecmds.cpp
    commands/indexcmds.cpp
    commands/copy.cpp
    commands/copy_from.cpp
    commands/copy_to.cpp
    commands/vacuum.cpp
    commands/analyze.cpp
    commands/sequence.cpp
    commands/view.cpp
    commands/trigger.cpp
    commands/explain.cpp
    commands/dbcommands.cpp
    commands/schemacmds.cpp
    commands/functioncmds.cpp
    commands/typecmds.cpp
    commands/operatorcmds.cpp
    commands/opclasscmds.cpp
    commands/aggregatecmds.cpp
    commands/tablespace.cpp
    server/postmaster.cpp
    server/bootstrap.cpp
    server/options.cpp
    server/guc.cpp
    server/auxprocess.cpp
    server/interrupt.cpp
    server/fork_process.cpp
    server/bgwriter.cpp
    server/checkpointer.cpp
    server/startup.cpp
    server/walwriter.cpp
    server/autovacuum.cpp
    server/pgarch.cpp
    server/syslogger.cpp
    server/bgworker.cpp
    server/shell_archive.cpp
    tools/psql_client.cpp
    tools/psql_command.cpp
    tools/psql_describe.cpp
    # Phase 15.20 (P2 top-level directories: lib/partitioning/regex/replication/statistics/tsearch)
    lib/dshash.cpp
    lib/ilist.cpp
    lib/rbtree.cpp
    lib/integerset.cpp
    lib/bloom.cpp
    lib/hyperloglog.cpp
    lib/binaryheap.cpp
    lib/pairingheap.cpp
    partitioning/partbounds.cpp
    partitioning/partdesc.cpp
    partitioning/partprune.cpp
    regex/regcomp.cpp
    regex/regexec.cpp
    regex/regfree.cpp
    regex/regerror.cpp
    regex/regprefix.cpp
    regex/regexport.cpp
    replication/replutil.cpp
    replication/walsender.cpp
    replication/walsenderfuncs.cpp
    replication/walreceiver.cpp
    replication/slot.cpp
    replication/slotfuncs.cpp
    replication/logical.cpp
    replication/worker.cpp
    replication/origin.cpp
    replication/launcher.cpp
    replication/syncrep.cpp
    replication/backup.cpp
    statistics/extended_stats.cpp
    statistics/mvdistinct.cpp
    statistics/mvdependencies.cpp
    statistics/mvdistinct_sample.cpp
    tsearch/dict.cpp
    tsearch/ispell.cpp
    tsearch/thesaurus.cpp
    tsearch/to_tsquery.cpp
    tsearch/to_tsvector.cpp
    tsearch/ts_typanalyze.cpp
    tsearch/ts_utils.cpp
    tsearch/tsquery_parser.cpp
    tsearch/tsvector_parser.cpp
    tsearch/wparser.cpp
)

add_library(pgcpp_core STATIC ${PGCPP_CORE_SOURCES})

target_compile_features(pgcpp_core PUBLIC cxx_std_20)
target_include_directories(pgcpp_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/parser
)

# Apply warning flags from the root CMakeLists.txt
target_compile_options(pgcpp_core PRIVATE -Wall -Wextra -Wpedantic)

# Generated Flex/Bison code triggers various warnings; suppress them.
set_source_files_properties(
    ${BISON_pgcpp_parser_OUTPUTS}
    ${FLEX_pgcpp_scanner_OUTPUTS}
    PROPERTIES
        COMPILE_OPTIONS "-Wno-unused;-Wno-sign-compare;-Wno-old-style-cast"
)

# ---------------------------------------------------------------------------
# pgcpp_server — the standalone server executable (M12)
# ---------------------------------------------------------------------------
add_executable(pgcpp_server server/main.cpp)
target_compile_features(pgcpp_server PRIVATE cxx_std_20)
target_link_libraries(pgcpp_server PRIVATE pgcpp_core)
target_include_directories(pgcpp_server PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

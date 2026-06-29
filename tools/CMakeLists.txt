# tools/CMakeLists.txt - pgcpp command-line tools (M13)
#
# Builds the psql and initdb command-line executables.

# Helper: declare a CLI tool that links against pgcpp_core.
function(pgcpp_add_tool name source)
    add_executable(${name} ${source})
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_link_libraries(${name} PRIVATE pgcpp_core)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
endfunction()

# psql — interactive query client
pgcpp_add_tool(pgcpp_psql psql/psql.cpp)

# initdb — cluster initialization tool
pgcpp_add_tool(pgcpp_initdb initdb/initdb.cpp)

# Phase 15.23: psql completionization + admin toolchain
pgcpp_add_tool(pgcpp_pg_ctl      pg_ctl/pg_ctl.cpp)
pgcpp_add_tool(pgcpp_pg_dump     pg_dump/pg_dump.cpp)
pgcpp_add_tool(pgcpp_pg_restore  pg_restore/pg_restore.cpp)
pgcpp_add_tool(pgcpp_vacuumdb    vacuumdb/vacuumdb.cpp)
pgcpp_add_tool(pgcpp_reindexdb   reindexdb/reindexdb.cpp)
pgcpp_add_tool(pgcpp_clusterdb   clusterdb/clusterdb.cpp)
pgcpp_add_tool(pgcpp_createdb    createdb/createdb.cpp)
pgcpp_add_tool(pgcpp_dropdb      dropdb/dropdb.cpp)
pgcpp_add_tool(pgcpp_createuser  createuser/createuser.cpp)
pgcpp_add_tool(pgcpp_dropuser    dropuser/dropuser.cpp)
pgcpp_add_tool(pgcpp_pg_isready  pg_isready/pg_isready.cpp)
pgcpp_add_tool(pgcpp_pg_config   pg_config/pg_config.cpp)

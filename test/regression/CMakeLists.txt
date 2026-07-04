# test/regression/CMakeLists.txt — SQL regression test harness.
#
# Registers a `regression-test` target that boots a fresh pgcpp server,
# runs every <name>.sql in this directory against it via pgcpp_psql,
# and diffs the output against <name>.expected.out.
#
# Usage:
#   cmake --build build --target regression-test
#
# The runner handles cluster init, server start/stop, and cleanup. It exits
# non-zero if any test fails, so the target is CI-friendly.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# pgcpp_psql, pgcpp_initdb, pgcpp_pg_isready, and pgcpp_server are built
# by the tools/ and src/ subdirectories. We only need to declare the custom
# target; the runner script locates the binaries under the build tree.
add_custom_target(regression-test
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/runner.py
        --build-dir $<TARGET_FILE_DIR:pgcpp_psql>/..
        --source-dir ${CMAKE_SOURCE_DIR}
        --test-dir ${CMAKE_CURRENT_SOURCE_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS pgcpp_psql pgcpp_initdb pgcpp_pg_isready pgcpp_server
    COMMENT "Running pgcpp SQL regression tests"
    VERBATIM
)

# test/CMakeLists.txt - MyToyDB test harness
#
# Unit tests are organized under test/unit/<module>/. Each test executable is
# linked against mytoydb_core and gtest_main and registered with ctest.

# Preserve the existing benchmark directory (no CMake logic there).
# Only wire up unit tests under test/unit/.

include(GoogleTest)

# Helper function to add a unit test executable.
# Usage: mytoydb_add_test(<name> <source>)
function(mytoydb_add_test name source)
    add_executable(${name} ${source})
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_link_libraries(${name} PRIVATE mytoydb_core gtest gtest_main)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    # Use PRE_TEST discovery so the test binary is not run at build time.
    # This avoids failures when sanitizers (especially LeakSanitizer under
    # ptrace-based sandboxes) prevent the binary from executing during the
    # build phase.
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

# Phase 1 tests
mytoydb_add_test(memory_context_test unit/common/memory/memory_context_test.cpp)
mytoydb_add_test(elog_test unit/common/error/elog_test.cpp)

# Phase 2 tests (M2: containers)
mytoydb_add_test(node_test unit/common/containers/node_test.cpp)
mytoydb_add_test(list_test unit/common/containers/list_test.cpp)
mytoydb_add_test(string_info_test unit/common/containers/string_info_test.cpp)

# Phase 3 tests (M4: types)
mytoydb_add_test(builtins_test unit/types/builtins_test.cpp)
mytoydb_add_test(datetime_test unit/types/datetime_test.cpp)
mytoydb_add_test(string_funcs_test unit/types/string_funcs_test.cpp)

# Phase 3 tests (M3: catalog)
mytoydb_add_test(catalog_test unit/catalog/catalog_test.cpp)

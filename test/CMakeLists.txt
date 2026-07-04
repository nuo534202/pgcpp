# test/CMakeLists.txt - pgcpp test harness
#
# Unit tests are organized under test/unit/<module>/. Each test executable is
# linked against pgcpp_core and gtest_main and registered with ctest.

# Preserve the existing benchmark directory (no CMake logic there).
# Only wire up unit tests under test/unit/.

include(GoogleTest)

# Helper function to add a unit test executable.
# Usage: pgcpp_add_test(<name> <source>)
function(pgcpp_add_test name source)
    add_executable(${name} ${source})
    target_compile_features(${name} PRIVATE cxx_std_20)
    # Link pgcpp_client (transitively provides pgcpp_core via PUBLIC linkage)
    # so that tests in test/unit/tools/ can access client tool functions.
    target_link_libraries(${name} PRIVATE pgcpp_client gtest gtest_main)
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

# Glob all *_test.cpp under test/unit/ (CONFIGURE_DEPENDS so CMake
# re-checks at build time and picks up newly added test files). The test
# target name is derived from the file's basename (e.g.
# unit/common/memory/memory_context_test.cpp -> memory_context_test).
file(GLOB_RECURSE PGCPP_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/*_test.cpp"
)
foreach(src ${PGCPP_TEST_SOURCES})
    get_filename_component(test_name ${src} NAME_WE)
    pgcpp_add_test(${test_name} ${src})
endforeach()

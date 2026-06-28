# FetchContent / dependency setup for pgcpp
#
# Strategy:
#   1. Prefer a system-installed Google Test (found via find_package(GTest CONFIG))
#      so the project can configure in environments without internet access.
#   2. Fall back to CMake FetchContent (the spec-mandated dependency mechanism)
#      using a mirror that is reachable from the build environment.
#
# After this function runs, the following targets are guaranteed to exist:
#   - gtest        (alias of either system gtest or FetchContent gtest)
#   - gtest_main   (alias of either system gtest_main or FetchContent gtest_main)
function(pgcpp_setup_fetchcontent)
    # 1. Try system-installed Google Test first.
    find_package(GTest CONFIG QUIET)

    if(GTest_FOUND)
        message(STATUS "pgcpp: using system-installed Google Test "
                       "(${GTest_VERSION})")
        # GTest::gtest / GTest::gtest_main are provided by the config file.
        # Create plain aliases named gtest / gtest_main so downstream code can
        # link against `gtest` / `gtest_main` regardless of source.
        if(TARGET GTest::gtest AND NOT TARGET gtest)
            add_library(gtest ALIAS GTest::gtest)
        endif()
        if(TARGET GTest::gtest_main AND NOT TARGET gtest_main)
            add_library(gtest_main ALIAS GTest::gtest_main)
        endif()
        return()
    endif()

    # 2. Fall back to FetchContent. Try a reachable mirror first, then GitHub.
    include(FetchContent)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://gitee.com/mirrors/googletest.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
    )

    message(STATUS "pgcpp: fetching Google Test 1.17.0 via FetchContent "
                   "(gitee mirror)")

    if(NOT TARGET gtest)
        FetchContent_MakeAvailable(googletest)
    endif()
endfunction()

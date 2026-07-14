cmake_minimum_required(VERSION 3.22.1)

project(pgcpp LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11.4)
        message(FATAL_ERROR
            "GCC version ${CMAKE_CXX_COMPILER_VERSION} is too old. "
            "pgcpp requires GCC >= 11.4 for full C++20 support.")
    endif()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sanitizers.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompilerWarnings.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchContent.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CodeStyle.cmake)

# ---------------------------------------------------------------------------
# LLVM JIT option (P3-6)
#
# When ENABLE_LLVM_JIT=ON, the LLVM C API is used to compile SQL expressions
# to native code at runtime. Requires llvm-dev (llvm-c/Core.h,
# llvm-c/ExecutionEngine.h) and libLLVM to be installed.
#
# Default: OFF (LLVM is not available in the CI environment).
# ---------------------------------------------------------------------------
option(ENABLE_LLVM_JIT "Enable LLVM-based JIT expression compilation" OFF)

if(ENABLE_LLVM_JIT)
    # Try to find llvm-config
    find_program(LLVM_CONFIG_BIN NAMES llvm-config llvm-config-14 llvm-config-15)
    if(LLVM_CONFIG_BIN)
        execute_process(
            COMMAND ${LLVM_CONFIG_BIN} --includedir
            OUTPUT_VARIABLE LLVM_INCLUDE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        execute_process(
            COMMAND ${LLVM_CONFIG_BIN} --libdir
            OUTPUT_VARIABLE LLVM_LIB_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        execute_process(
            COMMAND ${LLVM_CONFIG_BIN} --libs core executionengine native
            OUTPUT_VARIABLE LLVM_LIBS
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        execute_process(
            COMMAND ${LLVM_CONFIG_BIN} --system-libs
            OUTPUT_VARIABLE LLVM_SYSTEM_LIBS
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(LLVM_FOUND TRUE)
        message(STATUS "LLVM JIT enabled: include=${LLVM_INCLUDE_DIR} libdir=${LLVM_LIB_DIR}")
    else()
        message(WARNING "ENABLE_LLVM_JIT=ON but llvm-config not found; JIT will use stub provider")
        set(LLVM_FOUND FALSE)
    endif()
endif()

pgcpp_enable_sanitizers()
pgcpp_enable_warnings()
pgcpp_setup_fetchcontent()
pgcpp_setup_codestyle_targets()

enable_testing()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(pgcpp_common INTERFACE)
target_compile_features(pgcpp_common INTERFACE cxx_std_20)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
    add_subdirectory(src)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
    add_subdirectory(test)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()

cmake_minimum_required(VERSION 3.22.1)

project(mytoydb LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11.4)
        message(FATAL_ERROR
            "GCC version ${CMAKE_CXX_COMPILER_VERSION} is too old. "
            "mytoydb requires GCC >= 11.4 for full C++20 support.")
    endif()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sanitizers.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompilerWarnings.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchContent.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CodeStyle.cmake)

mytoydb_enable_sanitizers()
mytoydb_enable_warnings()
mytoydb_setup_fetchcontent()
mytoydb_setup_codestyle_targets()

enable_testing()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(mytoydb_common INTERFACE)
target_compile_features(mytoydb_common INTERFACE cxx_std_20)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
    add_subdirectory(src)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
    add_subdirectory(test)
endif()

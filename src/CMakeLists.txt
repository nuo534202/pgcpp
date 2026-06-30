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

# Glob all C++ sources under src/ (CONFIGURE_DEPENDS so CMake re-checks
# the glob at build time and re-configures when files are added/removed).
file(GLOB_RECURSE PGCPP_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
# server/main.cpp is the pgcpp_server entry point (has main()), not part
# of the pgcpp_core library — exclude it so it is only compiled via the
# pgcpp_server target below.
list(REMOVE_ITEM PGCPP_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/server/main.cpp"
)
# Append Flex/Bison generated sources (live in the build tree, not src/).
list(APPEND PGCPP_CORE_SOURCES
    ${BISON_pgcpp_parser_OUTPUTS}
    ${FLEX_pgcpp_scanner_OUTPUTS}
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

# src/CMakeLists.txt - pgcpp source library
#
# Builds the pgcpp_core static library from all module sources. New source
# files are added here as modules are converted from PostgreSQL C to C++20.

# ---------------------------------------------------------------------------
# Bison — generate parser for the SQL parser (M5)
# ---------------------------------------------------------------------------
find_package(BISON 3.0 REQUIRED)

# Ensure the parser output directory exists (BISON_TARGET does not create it).
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/parser)

# Bison generates parser.cpp and parser.hpp from gram.yy
# Use C++ skeleton (lalr1.cc) for clean C++ integration.
# -Wno-other suppresses "rule useless" / "nonterminal useless" / "type clash"
#   warnings that are inherent to PostgreSQL's large grammar and not actionable.
BISON_TARGET(
    pgcpp_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/gram.yy
    ${CMAKE_CURRENT_BINARY_DIR}/parser/gram.tab.cpp
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser/gram.tab.hpp
    COMPILE_FLAGS "-Wno-conflicts-sr -Wno-conflicts-rr -Wno-other"
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
# src/tools/*.cpp are client-side tool library functions (psql, pg_dump,
# etc.). They are excluded from pgcpp_core (server library) and built into
# a separate pgcpp_client static library below (D-4 layering fix).
file(GLOB PGCPP_CLIENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.cpp"
)
list(REMOVE_ITEM PGCPP_CORE_SOURCES ${PGCPP_CLIENT_SOURCES})
# Append Bison generated source (lives in the build tree, not src/).
list(APPEND PGCPP_CORE_SOURCES
    ${BISON_pgcpp_parser_OUTPUTS}
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

# Generated Bison code triggers various warnings; suppress them.
set_source_files_properties(
    ${BISON_pgcpp_parser_OUTPUTS}
    PROPERTIES
        COMPILE_OPTIONS "-Wno-unused;-Wno-sign-compare;-Wno-old-style-cast;-Wno-conversion;-Wno-shadow;-Wno-null-dereference"
)

# ---------------------------------------------------------------------------
# pgcpp_client — client-side tool library (M13)
#
# Contains the library functions for psql, pg_dump, pg_isready, etc.
# (located in src/tools/*.cpp). These are linked into the thin CLI
# executables in tools/<name>/<name>.cpp. Kept separate from pgcpp_core
# so the server library does not carry client-side code (D-4 fix).
# ---------------------------------------------------------------------------
add_library(pgcpp_client STATIC ${PGCPP_CLIENT_SOURCES})
target_compile_features(pgcpp_client PUBLIC cxx_std_20)
target_include_directories(pgcpp_client PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(pgcpp_client PUBLIC pgcpp_core)
target_compile_options(pgcpp_client PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# pgcpp_server — the standalone server executable (M12)
# ---------------------------------------------------------------------------
add_executable(pgcpp_server server/main.cpp)
target_compile_features(pgcpp_server PRIVATE cxx_std_20)
target_link_libraries(pgcpp_server PRIVATE pgcpp_core)
target_include_directories(pgcpp_server PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

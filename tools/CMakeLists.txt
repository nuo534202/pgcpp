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

# Glob each tool's .cpp under tools/<name>/<name>.cpp (CONFIGURE_DEPENDS so
# CMake re-checks at build time and picks up newly added tools). The tool
# target name is pgcpp_<basename> (e.g. psql/psql.cpp -> pgcpp_psql).
file(GLOB PGCPP_TOOL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp"
)
foreach(src ${PGCPP_TOOL_SOURCES})
    get_filename_component(tool_basename ${src} NAME_WE)
    pgcpp_add_tool(pgcpp_${tool_basename} ${src})
endforeach()

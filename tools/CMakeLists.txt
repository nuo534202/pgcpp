# tools/CMakeLists.txt - pgcpp command-line tools (M13)
#
# Builds the psql and initdb command-line executables.

# psql — interactive query client
add_executable(pgcpp_psql psql/psql.cpp)
target_compile_features(pgcpp_psql PRIVATE cxx_std_20)
target_link_libraries(pgcpp_psql PRIVATE pgcpp_core)
target_include_directories(pgcpp_psql PRIVATE ${CMAKE_SOURCE_DIR}/include)

# initdb — cluster initialization tool
add_executable(pgcpp_initdb initdb/initdb.cpp)
target_compile_features(pgcpp_initdb PRIVATE cxx_std_20)
target_link_libraries(pgcpp_initdb PRIVATE pgcpp_core)
target_include_directories(pgcpp_initdb PRIVATE ${CMAKE_SOURCE_DIR}/include)

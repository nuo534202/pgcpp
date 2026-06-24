# tools/CMakeLists.txt - MyToyDB command-line tools (M13)
#
# Builds the psql and initdb command-line executables.

# psql — interactive query client
add_executable(mytoydb_psql psql/psql.cpp)
target_compile_features(mytoydb_psql PRIVATE cxx_std_20)
target_link_libraries(mytoydb_psql PRIVATE mytoydb_core)
target_include_directories(mytoydb_psql PRIVATE ${CMAKE_SOURCE_DIR}/include)

# initdb — cluster initialization tool
add_executable(mytoydb_initdb initdb/initdb.cpp)
target_compile_features(mytoydb_initdb PRIVATE cxx_std_20)
target_link_libraries(mytoydb_initdb PRIVATE mytoydb_core)
target_include_directories(mytoydb_initdb PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Code style targets (clang-format / clang-tidy) for pgcpp
#
# Provides custom targets:
#   format        - run clang-format -i on all source files
#   format-check  - run clang-format --dry-run --Werror on all source files
#   tidy          - run clang-tidy on src/*.cpp files
#
# When there are no source files yet (early in the project), the targets print
# a status message instead of invoking the tools with an empty file list (which
# would cause clang-format to read from stdin and hang).
function(pgcpp_setup_codestyle_targets)
    find_program(CLANG_FORMAT_BIN clang-format)
    find_program(CLANG_TIDY_BIN clang-tidy)

    file(GLOB_RECURSE PGCPP_FORMAT_FILES
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.h"
        "${CMAKE_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_SOURCE_DIR}/include/*.cpp"
        "${CMAKE_SOURCE_DIR}/include/*.h"
        "${CMAKE_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_SOURCE_DIR}/test/*.cpp"
        "${CMAKE_SOURCE_DIR}/test/*.h"
        "${CMAKE_SOURCE_DIR}/test/*.hpp"
    )

    file(GLOB_RECURSE PGCPP_TIDY_FILES
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
    )

    list(JOIN PGCPP_FORMAT_FILES " " PGCPP_FORMAT_FILES_STR)
    list(JOIN PGCPP_TIDY_FILES " " PGCPP_TIDY_FILES_STR)

    if(CLANG_FORMAT_BIN)
        if(PGCPP_FORMAT_FILES)
            add_custom_target(format
                COMMAND ${CLANG_FORMAT_BIN} -i ${PGCPP_FORMAT_FILES}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Running clang-format -i on source files"
                VERBATIM
            )

            add_custom_target(format-check
                COMMAND ${CLANG_FORMAT_BIN} --dry-run --Werror ${PGCPP_FORMAT_FILES}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Checking source files with clang-format (dry-run)"
                VERBATIM
            )
        else()
            add_custom_target(format
                COMMAND ${CMAKE_COMMAND} -E echo "No source files to format yet."
                COMMENT "No source files to format yet."
            )
            add_custom_target(format-check
                COMMAND ${CMAKE_COMMAND} -E echo "No source files to check yet."
                COMMENT "No source files to check yet."
            )
        endif()
    else()
        message(STATUS "clang-format not found; skipping 'format' and 'format-check' targets")
    endif()

    if(CLANG_TIDY_BIN)
        if(PGCPP_TIDY_FILES)
            add_custom_target(tidy
                COMMAND ${CLANG_TIDY_BIN} ${PGCPP_TIDY_FILES}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Running clang-tidy on src/ .cpp files"
                VERBATIM
            )
        else()
            add_custom_target(tidy
                COMMAND ${CMAKE_COMMAND} -E echo "No source files to tidy yet."
                COMMENT "No source files to tidy yet."
            )
        endif()
    else()
        message(STATUS "clang-tidy not found; skipping 'tidy' target")
    endif()
endfunction()

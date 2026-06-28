function(pgcpp_enable_warnings)
    option(PGCPP_WERROR "Turn compiler warnings into errors" OFF)

    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wconversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
    )

    if(PGCPP_WERROR)
        add_compile_options(-Werror)
    endif()
endfunction()

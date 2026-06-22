function(mytoydb_enable_sanitizers)
    option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
    option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
    option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

    if(ENABLE_ASAN AND ENABLE_TSAN)
        message(FATAL_ERROR
            "AddressSanitizer and ThreadSanitizer are mutually exclusive. "
            "Enable only one of ENABLE_ASAN or ENABLE_TSAN.")
    endif()

    if(ENABLE_ASAN)
        message(STATUS "AddressSanitizer enabled")
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()

    if(ENABLE_TSAN)
        message(STATUS "ThreadSanitizer enabled")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()

    if(ENABLE_UBSAN)
        message(STATUS "UndefinedBehaviorSanitizer enabled")
        add_compile_options(
            -fsanitize=undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=undefined)
        add_link_options(-fsanitize=undefined)
    endif()
endfunction()

include_guard(GLOBAL)

function(iot_engine_configure_target target)
    if(MSVC)
        target_compile_options("${target}" PRIVATE /utf-8 /Zc:__cplusplus)
    endif()
endfunction()

function(iot_engine_add_test name)
    set(options LARGE NO_PROJECT_INCLUDE)
    set(multi_value_args
        SOURCES
        LIBRARIES
        INCLUDE_DIRECTORIES
        DEFINITIONS
        MSVC_DEFINITIONS)
    cmake_parse_arguments(PARSE_ARGV 1 arg
        "${options}" "" "${multi_value_args}")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments for ${name}-test: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "${name}-test requires at least one source")
    endif()

    set(target "${name}-test")
    add_executable("${target}" ${arg_SOURCES})
    if(NOT arg_NO_PROJECT_INCLUDE)
        target_include_directories("${target}" PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    if(arg_INCLUDE_DIRECTORIES)
        target_include_directories("${target}" PRIVATE
            ${arg_INCLUDE_DIRECTORIES})
    endif()
    if(arg_LIBRARIES)
        target_link_libraries("${target}" PRIVATE ${arg_LIBRARIES})
    endif()
    if(arg_DEFINITIONS)
        target_compile_definitions("${target}" PRIVATE ${arg_DEFINITIONS})
    endif()

    iot_engine_configure_target("${target}")
    if(MSVC)
        if(arg_LARGE)
            target_compile_options("${target}" PRIVATE /bigobj /FS)
        endif()
        if(arg_MSVC_DEFINITIONS)
            target_compile_definitions("${target}" PRIVATE
                ${arg_MSVC_DEFINITIONS})
        endif()
    endif()
    add_test(NAME "${name}" COMMAND "${target}")
endfunction()

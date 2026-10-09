cmake_minimum_required(VERSION 3.28)
set(prefix "${root}/${config}")
set(binary "${prefix}/build")
file(MAKE_DIRECTORY "${binary}" "${prefix}/include" "${prefix}/lib")

function(run)
    execute_process(COMMAND ${ARGV} WORKING_DIRECTORY "${binary}" COMMAND_ERROR_IS_FATAL ANY)
endfunction()

function(copy_library name)
    foreach(candidate IN ITEMS "${prefix}/lib/${name}.lib" "${prefix}/lib/lib${name}.lib"
            "${prefix}/lib/lib${name}.a" "${prefix}/lib/${name}_static.lib")
        if(EXISTS "${candidate}")
            file(COPY_FILE "${candidate}" "${prefix}/lib/${library_prefix}iot_${name}${library_suffix}" ONLY_IF_DIFFERENT)
            # Upstream pkg-config files use -l<name>, including on MSVC.
            set(canonical "${prefix}/lib/${library_prefix}${name}${library_suffix}")
            if(NOT candidate STREQUAL canonical)
                file(COPY_FILE "${candidate}" "${canonical}" ONLY_IF_DIFFERENT)
            endif()
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "Native dependency did not produce ${name} in ${prefix}/lib")
endfunction()

function(shell_path input output)
    if(WIN32)
        execute_process(COMMAND "${bash}" -c "cygpath -u \"$1\"" path "${input}"
            OUTPUT_VARIABLE converted OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        set(${output} "${converted}" PARENT_SCOPE)
    else()
        set(${output} "${input}" PARENT_SCOPE)
    endif()
endfunction()

if(kind STREQUAL "srt")
    set(ssl "${openssl_root}/${config}")
    set(args -S "${source}" -B "${binary}" -G "${generator}"
        "-DCMAKE_BUILD_TYPE=${config}" "-DCMAKE_C_COMPILER=${compiler}" "-DCMAKE_CXX_COMPILER=${cxx}"
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>"
        "-DCMAKE_INSTALL_PREFIX=${prefix}" -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_POLICY_DEFAULT_CMP0077=NEW -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
        -DENABLE_STATIC=ON -DENABLE_SHARED=OFF -DENABLE_APPS=OFF -DENABLE_UNITTESTS=OFF
        -DENABLE_BONDING=OFF -DUSE_OPENSSL_PC=OFF "-DOPENSSL_ROOT_DIR=${ssl}" -DOPENSSL_USE_STATIC_LIBS=ON)
    if(platform)
        list(APPEND args -A "${platform}")
    endif()
    run("${CMAKE_COMMAND}" ${args})
    run("${CMAKE_COMMAND}" --build "${binary}" --config "${config}" "-j${jobs}")
    run("${CMAKE_COMMAND}" --install "${binary}" --config "${config}")
    copy_library(srt)
elseif(kind STREQUAL "x264" OR kind STREQUAL "ffmpeg")
    shell_path("${source}/configure" configure)
    shell_path("${prefix}" unix_prefix)
    get_filename_component(compiler_dir "${compiler}" DIRECTORY)
    if(WIN32)
        set(ENV{PATH} "${compiler_dir};$ENV{PATH}")
        set(ENV{CC} cl)
        set(ENV{CXX} cl)
        if(config STREQUAL "Debug")
            set(runtime "-MTd")
        else()
            set(runtime "-MT")
        endif()
    else()
        set(ENV{CC} "${compiler}")
        set(ENV{CXX} "${cxx}")
        set(runtime "-fPIC")
    endif()
    if(kind STREQUAL "x264")
        set(args "--prefix=${unix_prefix}" --enable-static --disable-cli --enable-pic
            --disable-lavf --disable-swscale --disable-avs --disable-ffms --disable-gpac
            --disable-lsmash --disable-opencl "--extra-cflags=${runtime}")
        if(WIN32)
            list(APPEND args --host=x86_64-w64-mingw32)
        endif()
        if(config STREQUAL "Debug")
            list(APPEND args --enable-debug)
        endif()
        run("${bash}" "${configure}" ${args})
        run("${make}" "-j${jobs}")
        run("${make}" install)
        copy_library(x264)
    else()
        if(WIN32)
            # Localized MSVC banners can have text before "Microsoft".
            file(READ "${source}/configure" configure_source)
            string(REPLACE "grep -q ^Microsoft" "grep -qi Microsoft" configure_source "${configure_source}")
            string(REPLACE "grep ^Microsoft" "grep -i Microsoft" configure_source "${configure_source}")
            string(REPLACE [=[grep -i Microsoft | head -n1 | tr -d '\r']=]
                [=[grep -i Microsoft | head -n1 | LC_ALL=C tr -cd '\11\12\40-\176']=]
                configure_source "${configure_source}")
            file(WRITE "${source}/configure" "${configure_source}")
        endif()
        shell_path("${x264_root}/${config}" x264)
        shell_path("${srt_root}/${config}" srt)
        shell_path("${openssl_root}/${config}" ssl)
        # OpenSSL's Windows install_dev target omits pkg-config files. Adapt
        # its generated export metadata to the installed prefix and library names.
        file(MAKE_DIRECTORY "${binary}/pkgconfig")
        foreach(package IN ITEMS openssl libssl libcrypto)
            if(EXISTS "${openssl_root}/${config}/build/exporters/${package}.pc")
                file(READ "${openssl_root}/${config}/build/exporters/${package}.pc" metadata)
                string(REGEX REPLACE "prefix=[^\n]*" "prefix=${ssl}" metadata "${metadata}")
                if(WIN32)
                    string(REPLACE "-lssl" "-llibssl" metadata "${metadata}")
                    string(REPLACE "-lcrypto" "-llibcrypto" metadata "${metadata}")
                endif()
                file(WRITE "${binary}/pkgconfig/${package}.pc" "${metadata}")
            endif()
        endforeach()
        shell_path("${binary}/pkgconfig" openssl_metadata)
        set(ENV{PKG_CONFIG_PATH} "${x264}/lib/pkgconfig:${srt}/lib/pkgconfig:${openssl_metadata}:${ssl}/lib/pkgconfig")
        set(args "--prefix=${unix_prefix}" --enable-static --disable-shared --enable-pic
            --disable-programs --disable-doc --disable-autodetect --enable-runtime-cpudetect
            --enable-gpl --enable-version3 --enable-libx264 --enable-libsrt --enable-openssl
            "--pkg-config=${pkgconfig}" --pkg-config-flags=--static
            "--extra-cxxflags=${runtime}"
            "--extra-cflags=${runtime} -I${x264}/include -I${srt}/include -I${ssl}/include")
        if(WIN32)
            list(APPEND args --toolchain=msvc --target-os=win32 --arch=x86_64
                --enable-w32threads --enable-d3d11va --enable-dxva2 --enable-mediafoundation
                "--extra-ldflags=-libpath:${x264_root}/${config}/lib -libpath:${srt_root}/${config}/lib -libpath:${openssl_root}/${config}/lib")
        else()
            list(APPEND args --enable-pthreads
                "--extra-ldflags=-L${x264}/lib -L${srt}/lib -L${ssl}/lib")
        endif()
        if(config STREQUAL "Debug")
            list(APPEND args --enable-debug)
        endif()
        run("${bash}" "${configure}" ${args})
        run("${make}" "-j${jobs}")
        run("${make}" install)
        foreach(library IN ITEMS avcodec avformat avfilter avutil swresample swscale)
            copy_library("${library}")
        endforeach()
    endif()
else()
    message(FATAL_ERROR "Unknown native dependency: ${kind}")
endif()
file(TOUCH "${prefix}/complete.stamp")

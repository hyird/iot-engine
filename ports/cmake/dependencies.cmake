include_guard(GLOBAL)
include(FetchContent)
include(ProcessorCount)

function(iot_native_dependency name)
    cmake_parse_arguments(PARSE_ARGV 1 dep "" "VERSION" "LIBRARIES;DEPENDS;ARGUMENTS")
    FetchContent_MakeAvailable(iot_${name})
    FetchContent_GetProperties(iot_${name} SOURCE_DIR source)
    string(SHA256 signature "${dep_VERSION};${source};${CMAKE_C_COMPILER};${CMAKE_CXX_COMPILER};${CMAKE_C_COMPILER_VERSION};${dep_ARGUMENTS}")
    string(SUBSTRING "${signature}" 0 12 signature)
    set(root "${FETCHCONTENT_BASE_DIR}/iot-${name}-${signature}")
    foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY "${root}/${config}/include" "${root}/${config}/lib")
    endforeach()
    ProcessorCount(jobs)
    if(NOT jobs)
        set(jobs 1)
    endif()
    set(configuration "$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,Release>")
    set(outputs)
    foreach(library IN LISTS dep_LIBRARIES)
        list(APPEND outputs "${root}/${configuration}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}iot_${library}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    endforeach()
    add_custom_command(OUTPUT "${root}/${configuration}/complete.stamp"
        BYPRODUCTS ${outputs}
        COMMAND "${CMAKE_COMMAND}" "-Dkind=${name}" "-Dsource=${source}" "-Droot=${root}"
            "-Dconfig=${configuration}" "-Djobs=${jobs}" "-Dcompiler=${CMAKE_C_COMPILER}"
            "-Dcxx=${CMAKE_CXX_COMPILER}" "-Dgenerator=${CMAKE_GENERATOR}"
            "-Dplatform=${CMAKE_GENERATOR_PLATFORM}" "-Dlibrary_prefix=${CMAKE_STATIC_LIBRARY_PREFIX}"
            "-Dlibrary_suffix=${CMAKE_STATIC_LIBRARY_SUFFIX}" ${dep_ARGUMENTS}
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/native-build.cmake"
        DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/native-build.cmake" ${dep_DEPENDS}
        VERBATIM USES_TERMINAL)
    add_custom_target(iot_build_${name} DEPENDS "${root}/${configuration}/complete.stamp")
    foreach(library IN LISTS dep_LIBRARIES)
        add_library(iot_${library} STATIC IMPORTED GLOBAL)
        set_target_properties(iot_${library} PROPERTIES
            IMPORTED_CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel"
            IMPORTED_LOCATION "${root}/Release/lib/${CMAKE_STATIC_LIBRARY_PREFIX}iot_${library}${CMAKE_STATIC_LIBRARY_SUFFIX}"
            INTERFACE_INCLUDE_DIRECTORIES "${root}/${configuration}/include")
        foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
            string(TOUPPER "${config}" upper)
            set_property(TARGET iot_${library} PROPERTY IMPORTED_LOCATION_${upper}
                "${root}/${config}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}iot_${library}${CMAKE_STATIC_LIBRARY_SUFFIX}")
        endforeach()
        add_dependencies(iot_${library} iot_build_${name})
    endforeach()
    set(iot_${name}_ROOT "${root}" PARENT_SCOPE)
    set(iot_${name}_SOURCE_DIR "${source}" PARENT_SCOPE)
endfunction()

function(iot_engine_fetch_dependencies)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

    # Media configure checks need headers. Bootstrap the same native build
    # owned by Ruvia, then link every consumer to its configuration-aware targets.
    get_property(openssl_root GLOBAL PROPERTY RUVIA_OPENSSL_ROOT)
    FetchContent_GetProperties(ruvia_openssl SOURCE_DIR openssl_source)
    find_package(Perl REQUIRED)
    ProcessorCount(jobs)
    if(WIN32)
        find_program(openssl_make NAMES jom REQUIRED)
        set(openssl_target VC-WIN64A)
    else()
        find_program(openssl_make NAMES gmake make REQUIRED)
        set(openssl_target "")
    endif()
    if(NOT EXISTS "${openssl_root}/Release/complete.stamp")
        execute_process(COMMAND "${CMAKE_COMMAND}" -Dkind=openssl "-Dsource=${openssl_source}"
            "-Droot=${openssl_root}" -Dconfig=Release "-Djobs=${jobs}"
            "-Dcompiler=${CMAKE_C_COMPILER}" "-Dgenerator=${CMAKE_GENERATOR}"
            "-Dlibrary_prefix=${CMAKE_STATIC_LIBRARY_PREFIX}" "-Dlibrary_suffix=${CMAKE_STATIC_LIBRARY_SUFFIX}"
            "-Dperl=${PERL_EXECUTABLE}" "-Dmake=${openssl_make}" "-Dopenssl_target=${openssl_target}"
            -P "${ruvia_SOURCE_DIR}/cmake/RuviaNativeBuild.cmake"
            COMMAND_ERROR_IS_FATAL ANY)
    endif()
    if(NOT TARGET OpenSSL::Crypto)
        add_library(OpenSSL::Crypto ALIAS ruvia_openssl_crypto)
        add_library(OpenSSL::SSL ALIAS ruvia_openssl_ssl)
    endif()
    set(OPENSSL_ROOT_DIR "${openssl_root}/Release" CACHE PATH "" FORCE)
    set(OPENSSL_USE_STATIC_LIBS ON)
    find_package(OpenSSL 4 REQUIRED)

    FetchContent_GetProperties(ruvia_zlib SOURCE_DIR zlib_source)
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()
    set(ZLIB_INCLUDE_DIR "${zlib_source}" CACHE PATH "" FORCE)
    set(ZLIB_LIBRARY zlibstatic CACHE STRING "" FORCE)

    FetchContent_Declare(iot_abseil
        URL https://github.com/abseil/abseil-cpp/archive/refs/tags/20260107.1.tar.gz
        URL_HASH SHA512=f5012885d6b6844a9cf5ed92ad5468b8757db33dfe1364bfb232fff928e06c550c7eb4557f45186a8ac4d18b178df9be267681abab4a6de40823b574afbe9960
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_Declare(iot_protobuf
        URL https://github.com/protocolbuffers/protobuf/archive/refs/tags/v33.4.tar.gz
        URL_HASH SHA512=540059a93721447cf4723bcca06e91c43a4399cb366c05bf84e9d8e2c439f3107ba17803f9d912549b54c471f2dcc4c9fc834145ec441dff31ca24f9a3543aa9
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_Declare(iot_pugixml
        URL https://github.com/zeux/pugixml/archive/refs/tags/v1.16.tar.gz
        URL_HASH SHA512=a550e18c998d4eb7eaadcfe4db7e5616b95e04315fb571563625be1882a54a8e9ac710de79d9451f9dddea8600c898487f2d067d0916dd7abc6e3e29a1624e0b
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_Declare(iot_spdlog
        URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA512=8df117055d19ff21c9c9951881c7bdf27cc0866ea3a4aa0614b2c3939cedceab94ac9abaa63dc4312b51562b27d708cb2f014c68c603fd1c1051d3ed5c1c3087
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_Declare(iot_srtp
        URL https://github.com/cisco/libsrtp/archive/refs/tags/v2.8.0.tar.gz
        URL_HASH SHA512=6768f7976e5cc14a3bf2e9fc32042cab0b964f616fe5654516643a649a5d5f2b9ecb9e996467dd6d337777a9051b83a6e95f3cdc27e945062ce6da1cf8a2d462
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_Declare(iot_usrsctp
        URL https://github.com/sctplab/usrsctp/archive/refs/tags/0.9.5.0.tar.gz
        URL_HASH SHA512=7b28706449f9365ba9750fd39925e7171516a1e3145d123ec69a12486637ae2393ad4c587b056403298dc13c149f0b01a262cbe4852abca42e425d7680c77ee3
        EXCLUDE_FROM_ALL SYSTEM)
    set(srt_version 1.5.6)
    set(x264_version 31e19f92f00c7003fa115047ce50978bc98c3a0d)
    set(ffmpeg_version 8.1.2)
    FetchContent_Declare(iot_srt
        URL https://github.com/Haivision/srt/archive/refs/tags/v${srt_version}.tar.gz
        URL_HASH SHA512=57641b35644b6bfa5998648fb808b615d11d8eab52fecb628a58414dbc87b1d781ea281c8ceef42a92f0c3796a05b41b8411bea95188ce751544f8195b7dbb66
        SOURCE_SUBDIR iot-no-cmake)
    FetchContent_Declare(iot_x264
        URL https://code.videolan.org/videolan/x264/-/archive/${x264_version}/x264-${x264_version}.tar.gz
        URL_HASH SHA512=707ff486677a1b5502d6d8faa588e7a03b0dee45491c5cba89341be4be23d3f2e48272c3b11d54cfc7be1b8bf4a3dfc3c3bb6d9643a6b5a2ed77539c85ecf294
        SOURCE_SUBDIR iot-no-cmake)
    FetchContent_Declare(iot_ffmpeg
        URL https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n${ffmpeg_version}.tar.gz
        URL_HASH SHA512=c72f4062aecc16d8b2b1e8678d5efe3af4cfaa0cc7c0997052248f9e499e60c2463acf07877cf3b78b246ce3e8078cb043e8d97e90a6b50d06af32ff7369a788
        SOURCE_SUBDIR iot-no-cmake)
    set(ABSL_PROPAGATE_CXX_STD ON)
    set(ABSL_BUILD_TESTING OFF)
    set(ABSL_MSVC_STATIC_RUNTIME ON)
    set(protobuf_BUILD_TESTS OFF)
    set(protobuf_INSTALL OFF)
    set(protobuf_BUILD_SHARED_LIBS OFF)
    set(protobuf_MSVC_STATIC_RUNTIME ON)
    set(protobuf_BUILD_PROTOC_BINARIES ON)
    set(protobuf_BUILD_LIBPROTOC ON)
    set(protobuf_LOCAL_DEPENDENCIES_ONLY ON)
    set(SPDLOG_BUILD_SHARED OFF)
    set(SPDLOG_FMT_EXTERNAL OFF)
    set(SPDLOG_BUILD_TESTS OFF)
    set(SPDLOG_BUILD_EXAMPLE OFF)
    set(PUGIXML_BUILD_TESTS OFF)
    FetchContent_MakeAvailable(iot_abseil iot_protobuf iot_pugixml iot_spdlog)
    if(MSVC)
        # Visual Studio otherwise compiles the large Protobuf projects serially.
        target_compile_options(libprotobuf PRIVATE /MP8)
        target_compile_options(libprotoc PRIVATE /MP8)
    endif()
    set(ENABLE_OPENSSL ON)
    set(ENABLE_WARNINGS_AS_ERRORS OFF)
    set(LIBSRTP_TEST_APPS OFF)
    set(TEST_APPS OFF)
    set(sctp_werror OFF)
    set(sctp_build_programs OFF)
    FetchContent_MakeAvailable(iot_srtp iot_usrsctp)
    target_compile_definitions(usrsctp PRIVATE timingsafe_bcmp=usrsctp_timingsafe_bcmp)
    file(MAKE_DIRECTORY "${iot_srtp_BINARY_DIR}/public/srtp2")
    configure_file("${iot_srtp_SOURCE_DIR}/include/srtp.h" "${iot_srtp_BINARY_DIR}/public/srtp2/srtp.h" COPYONLY)
    set(SRTP_INCLUDE_DIRS "${iot_srtp_BINARY_DIR}/public" CACHE PATH "" FORCE)
    set(SRTP_LIBRARIES srtp2 CACHE STRING "" FORCE)
    set(SCTP_INCLUDE_DIRS "${iot_usrsctp_SOURCE_DIR}/usrsctplib" CACHE PATH "" FORCE)
    set(SCTP_LIBRARIES usrsctp CACHE STRING "" FORCE)
    find_program(iot_bash NAMES bash REQUIRED)
    if(WIN32)
        get_filename_component(msys_bin "${iot_bash}" DIRECTORY)
        find_program(iot_make NAMES make HINTS "${msys_bin}" NO_DEFAULT_PATH REQUIRED)
        find_program(iot_pkgconfig NAMES pkg-config pkgconf HINTS "${msys_bin}" NO_DEFAULT_PATH REQUIRED)
    else()
        find_program(iot_make NAMES gmake make REQUIRED)
        find_program(iot_pkgconfig NAMES pkg-config pkgconf REQUIRED)
    endif()
    iot_native_dependency(x264 VERSION "${x264_version}" LIBRARIES x264
        ARGUMENTS "-Dbash=${iot_bash}" "-Dmake=${iot_make}" "-Dpkgconfig=${iot_pkgconfig}")
    iot_native_dependency(srt VERSION "${srt_version}" LIBRARIES srt DEPENDS ruvia_build_openssl
        ARGUMENTS "-Dopenssl_root=${openssl_root}")
    iot_native_dependency(ffmpeg VERSION "${ffmpeg_version}" LIBRARIES avcodec avformat avfilter avutil swresample swscale
        DEPENDS iot_build_x264 iot_build_srt ruvia_build_openssl
        ARGUMENTS "-Dbash=${iot_bash}" "-Dmake=${iot_make}" "-Dpkgconfig=${iot_pkgconfig}"
            "-Dx264_root=${iot_x264_ROOT}" "-Dsrt_root=${iot_srt_ROOT}" "-Dopenssl_root=${openssl_root}")
    target_link_libraries(iot_srt INTERFACE OpenSSL::SSL)
    target_link_libraries(iot_avformat INTERFACE iot_avcodec iot_avutil iot_srt OpenSSL::SSL)
    target_link_libraries(iot_avcodec INTERFACE iot_avutil iot_swresample iot_x264)
    target_link_libraries(iot_avfilter INTERFACE iot_avformat iot_avcodec iot_swscale iot_swresample iot_avutil)
    target_link_libraries(iot_swresample INTERFACE iot_avutil)
    target_link_libraries(iot_swscale INTERFACE iot_avutil)
    target_link_libraries(iot_avutil INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
    if(WIN32)
        target_link_libraries(iot_avutil INTERFACE bcrypt ole32 uuid ws2_32 secur32 user32 gdi32 mfplat mfuuid strmiids)
    else()
        target_link_libraries(iot_avutil INTERFACE m)
        target_link_libraries(iot_x264 INTERFACE Threads::Threads m)
    endif()
    foreach(library IN ITEMS avcodec avformat avfilter avutil swresample swscale)
        string(TOUPPER "${library}" upper)
        set(${upper}_LIBRARY "iot_${library}" CACHE STRING "" FORCE)
        set(${upper}_INCLUDE_DIR "${iot_ffmpeg_ROOT}/Release/include" CACHE PATH "" FORCE)
    endforeach()
    set(X264_LIBRARIES iot_x264 CACHE STRING "" FORCE)
    set(X264_INCLUDE_DIRS "${iot_x264_ROOT}/Release/include" CACHE PATH "" FORCE)
endfunction()
function(iot_engine_write_license_manifest)
    set(records)
    foreach(name IN ITEMS ruvia ruvia_asio ruvia_zlib ruvia_brotli ruvia_zstd ruvia_ngtcp2
            ruvia_openssl ruvia_postgresql ruvia_hiredis iot_abseil iot_protobuf iot_pugixml
            iot_spdlog iot_srtp iot_usrsctp iot_srt iot_x264 iot_ffmpeg nanopb faac_source zlmediakit)
        FetchContent_GetProperties(${name} SOURCE_DIR source POPULATED populated)
        if(populated)
            list(APPEND records "${name}|${source}")
        endif()
    endforeach()
    file(WRITE "${CMAKE_BINARY_DIR}/third-party-sources.cmake" "set(IOT_THIRD_PARTY_SOURCES\n")
    foreach(record IN LISTS records)
        file(APPEND "${CMAKE_BINARY_DIR}/third-party-sources.cmake" "  \"${record}\"\n")
    endforeach()
    file(APPEND "${CMAKE_BINARY_DIR}/third-party-sources.cmake" ")\n")
endfunction()

cmake_minimum_required(VERSION 3.24)

if(NOT CMAKE_TOOLCHAIN_FILE)
    if("$ENV{VCPKG_ROOT}" STREQUAL "")
        message(FATAL_ERROR "VCPKG_ROOT is not set")
    endif()
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VCPKG_ROOT_PATH)
    set(CMAKE_TOOLCHAIN_FILE
        "${VCPKG_ROOT_PATH}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
endif()

if(NOT "$ENV{VCPKG_DEFAULT_TRIPLET}" STREQUAL "")
    set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "")
endif()

if(NOT "$ENV{VCPKG_DEFAULT_HOST_TRIPLET}" STREQUAL "")
    set(VCPKG_HOST_TRIPLET "$ENV{VCPKG_DEFAULT_HOST_TRIPLET}" CACHE STRING "")
endif()

list(PREPEND VCPKG_OVERLAY_PORTS
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg-overlay-ports")

# ZLMediaKit and its Find*.cmake modules use CMAKE_BUILD_TYPE even with
# multi-config generators. Set the dependency-discovery configuration before
# project() loads the vcpkg toolchain, otherwise Visual Studio can bind Release
# targets to /MTd libraries from debug/lib.
if(CMAKE_GENERATOR MATCHES "Visual Studio|Xcode|Ninja Multi-Config")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING
        "Dependency-discovery configuration for multi-config generators" FORCE)
endif()

project(iot_engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CheckIncludeFile)
include(FetchContent)

# CMAKE_COMPILE_WARNING_AS_ERROR is a CI policy for targets owned by this
# repository. Do not propagate it into fetched third-party targets.
set(IOT_ENGINE_COMPILE_WARNING_AS_ERROR "${CMAKE_COMPILE_WARNING_AS_ERROR}")
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)

set(RUVIA_ENABLE_POSTGRESQL ON CACHE BOOL "" FORCE)
set(RUVIA_ENABLE_REDIS ON CACHE BOOL "" FORCE)
set(RUVIA_ENABLE_JWT ON CACHE BOOL "" FORCE)

FetchContent_Declare(ruvia
    GIT_REPOSITORY https://github.com/hyird/Ruvia.git
    # Last commit verified by this repository's Windows/MSVC build. Newer main
    # currently fails inside Ruvia's split database capability sources.
    GIT_TAG 10263676ed08316b458de6e15ae29cb3ab6583f3)
FetchContent_MakeAvailable(ruvia)

FetchContent_Declare(nanopb
    GIT_REPOSITORY https://github.com/nanopb/nanopb.git
    GIT_TAG cad3c18ef15a663e30e3e43e3a752b66378adec1
    # The server uses the standard C++ Protobuf runtime. Fetch nanopb only for the
    # exact nanopb.proto option schema shared with the OpenWrt nanopb generator.
    SOURCE_SUBDIR _runtime_sources_only)
FetchContent_MakeAvailable(nanopb)

# ZLMediaKit's C SDK exposes PCM-to-AAC input only when FAAC is present. FAAC
# does not ship a CMake build or a vcpkg port, so build its upstream static
# library directly from the source list used by its own MSVC project.
FetchContent_Declare(faac_source
    GIT_REPOSITORY https://github.com/knik0/faac.git
    GIT_TAG 3aec0c1cef9442fe2f8eb86a5b475c68e94f3306
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(faac_source)
add_library(faac STATIC
    "${faac_source_SOURCE_DIR}/libfaac/bitstream.c"
    "${faac_source_SOURCE_DIR}/libfaac/blockswitch.c"
    "${faac_source_SOURCE_DIR}/libfaac/channels.c"
    "${faac_source_SOURCE_DIR}/libfaac/fft.c"
    "${faac_source_SOURCE_DIR}/libfaac/filtbank.c"
    "${faac_source_SOURCE_DIR}/libfaac/frame.c"
    "${faac_source_SOURCE_DIR}/libfaac/huff2.c"
    "${faac_source_SOURCE_DIR}/libfaac/huffdata.c"
    "${faac_source_SOURCE_DIR}/libfaac/quantize.c"
    "${faac_source_SOURCE_DIR}/libfaac/stereo.c"
    "${faac_source_SOURCE_DIR}/libfaac/tns.c"
    "${faac_source_SOURCE_DIR}/libfaac/util.c")
target_include_directories(faac
    PUBLIC "${faac_source_SOURCE_DIR}/include"
    PRIVATE "${faac_source_SOURCE_DIR}/libfaac")
target_compile_definitions(faac PRIVATE PACKAGE_VERSION="1.31.1")
check_include_file("immintrin.h" IOT_ENGINE_HAVE_IMMINTRIN_H)
if(IOT_ENGINE_HAVE_IMMINTRIN_H)
    target_compile_definitions(faac PRIVATE HAVE_IMMINTRIN_H=1)
endif()
set_property(TARGET faac PROPERTY COMPILE_WARNING_AS_ERROR OFF)
if(MSVC)
    target_compile_definitions(faac PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_link_libraries(faac PUBLIC m)
endif()
set(FAAC_INCLUDE_DIR "${faac_source_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(FAAC_LIBRARY faac CACHE STRING "" FORCE)

# ZLMediaKit runs the GB28181 media data plane in its own EventPollerPool and
# WorkThreadPool. Link mk_api statically so the release remains one executable;
# linkage does not change the media worker isolation boundary.
set(ENABLE_API ON CACHE BOOL "" FORCE)
set(ENABLE_API_STATIC_LIB ON CACHE BOOL "" FORCE)
# These switches build separate ZLM executables/install surfaces, not embedded
# mk_api capabilities. Keep the application boundary on the C SDK only.
set(ENABLE_CXX_API OFF CACHE BOOL "" FORCE)
set(ENABLE_SERVER OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_PLAYER OFF CACHE BOOL "" FORCE)
# These are auxiliary integration/debug surfaces rather than media codecs or
# protocols. iot-engine owns persistence through Ruvia/PostgreSQL and does not
# expose ZLM's Python, MySQL, allocator, or C++ API boundaries.
set(ENABLE_MYSQL OFF CACHE BOOL "" FORCE)
set(ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
set(ENABLE_ASAN OFF CACHE BOOL "" FORCE)
set(ENABLE_MEM_DEBUG OFF CACHE BOOL "" FORCE)
set(ENABLE_JEMALLOC_STATIC OFF CACHE BOOL "" FORCE)
set(ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
set(ENABLE_FAAC ON CACHE BOOL "" FORCE)
set(ENABLE_FFMPEG ON CACHE BOOL "" FORCE)
set(ENABLE_HLS ON CACHE BOOL "" FORCE)
set(ENABLE_MP4 ON CACHE BOOL "" FORCE)
set(ENABLE_RTPPROXY ON CACHE BOOL "" FORCE)
set(ENABLE_SRT ON CACHE BOOL "" FORCE)
set(ENABLE_SCTP ON CACHE BOOL "" FORCE)
set(ENABLE_WEBRTC ON CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL ON CACHE BOOL "" FORCE)
set(ENABLE_X264 ON CACHE BOOL "" FORCE)
set(ENABLE_VIDEOSTACK ON CACHE BOOL "" FORCE)
set(ENABLE_OBJCOPY OFF CACHE BOOL "" FORCE)
set(DISABLE_REPORT ON CACHE BOOL "" FORCE)
FetchContent_Declare(zlmediakit
    GIT_REPOSITORY https://github.com/ZLMediaKit/ZLMediaKit.git
    GIT_TAG 79d795a767da85bbba821b871265fd87d853d808
    GIT_SHALLOW TRUE
    GIT_SUBMODULES
        3rdpart/ZLToolKit
        3rdpart/jsoncpp
        3rdpart/media-server
    GIT_SUBMODULES_RECURSE TRUE
    PATCH_COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=<SOURCE_DIR>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-zlmediakit.cmake")

FetchContent_MakeAvailable(zlmediakit)

set(CMAKE_COMPILE_WARNING_AS_ERROR "${IOT_ENGINE_COMPILE_WARNING_AS_ERROR}")
unset(IOT_ENGINE_COMPILE_WARNING_AS_ERROR)

get_target_property(IOT_ENGINE_ZLM_DEFINITIONS zlmediakit COMPILE_DEFINITIONS)
foreach(IOT_ENGINE_ZLM_FEATURE
        ENABLE_FAAC
        ENABLE_FFMPEG
        ENABLE_HLS
        ENABLE_MP4
        ENABLE_OPENSSL
        ENABLE_RTPPROXY
        ENABLE_SRT
        ENABLE_WEBRTC
        ENABLE_X264
        ENABLE_VIDEOSTACK)
    if(NOT "${IOT_ENGINE_ZLM_FEATURE}" IN_LIST IOT_ENGINE_ZLM_DEFINITIONS)
        message(FATAL_ERROR
            "Embedded ZLMediaKit feature was not enabled: ${IOT_ENGINE_ZLM_FEATURE}")
    endif()
endforeach()
get_target_property(IOT_ENGINE_ZLM_WEBRTC_DEFINITIONS webrtc COMPILE_DEFINITIONS)
if(NOT "ENABLE_SCTP" IN_LIST IOT_ENGINE_ZLM_WEBRTC_DEFINITIONS)
    message(FATAL_ERROR "Embedded ZLMediaKit WebRTC data channel support was not enabled")
endif()
unset(IOT_ENGINE_ZLM_DEFINITIONS)
unset(IOT_ENGINE_ZLM_WEBRTC_DEFINITIONS)
unset(IOT_ENGINE_ZLM_FEATURE)

if(ENABLE_API_STATIC_LIB)
    target_compile_definitions(mk_api INTERFACE MediaKitApi_STATIC)
endif()
if(MSVC)
    # mk_common.h only includes the generated export header when this is set.
    # Without it, static consumers are incorrectly compiled with dllimport.
    target_compile_definitions(mk_api INTERFACE GENERATE_EXPORT)
endif()

find_package(Protobuf CONFIG REQUIRED)
find_package(unofficial-brotli CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)

# Rebind Brotli to vcpkg's configuration-aware imported targets.
if(TARGET Brotli_lib
   AND TARGET unofficial::brotli::brotlidec
   AND TARGET unofficial::brotli::brotlienc)
    set_target_properties(Brotli_lib PROPERTIES
        INTERFACE_LINK_LIBRARIES
        "unofficial::brotli::brotlidec;unofficial::brotli::brotlienc")
endif()

set(EDGENODE_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/service/features/edge")
set(EDGENODE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/edgenode")
set(EDGENODE_PROTO_SOURCE "${EDGENODE_PROTO_DIR}/edge.proto")
set(WEB_TERMINAL_PROTO_SOURCE "${EDGENODE_PROTO_DIR}/terminal.proto")
set(EDGENODE_NANOPB_PROTO_SOURCE
    "${nanopb_SOURCE_DIR}/generator/proto/nanopb.proto")
set(EDGENODE_PROTO_CPP "${EDGENODE_GENERATED_DIR}/edge.pb.cc")
set(EDGENODE_PROTO_H "${EDGENODE_GENERATED_DIR}/edge.pb.h")
set(EDGENODE_NANOPB_PROTO_CPP "${EDGENODE_GENERATED_DIR}/nanopb.pb.cc")
set(EDGENODE_NANOPB_PROTO_H "${EDGENODE_GENERATED_DIR}/nanopb.pb.h")
set(WEB_TERMINAL_PROTO_CPP "${EDGENODE_GENERATED_DIR}/terminal.pb.cc")
set(WEB_TERMINAL_PROTO_H "${EDGENODE_GENERATED_DIR}/terminal.pb.h")
add_custom_command(
    OUTPUT
        "${EDGENODE_PROTO_CPP}"
        "${EDGENODE_PROTO_H}"
        "${EDGENODE_NANOPB_PROTO_CPP}"
        "${EDGENODE_NANOPB_PROTO_H}"
        "${WEB_TERMINAL_PROTO_CPP}"
        "${WEB_TERMINAL_PROTO_H}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EDGENODE_GENERATED_DIR}"
    COMMAND protobuf::protoc
            "--cpp_out=${EDGENODE_GENERATED_DIR}"
            "--proto_path=${EDGENODE_PROTO_DIR}"
            "--proto_path=${nanopb_SOURCE_DIR}/generator/proto"
            "${EDGENODE_PROTO_SOURCE}"
            "${WEB_TERMINAL_PROTO_SOURCE}"
            "${EDGENODE_NANOPB_PROTO_SOURCE}"
    DEPENDS
        "${EDGENODE_PROTO_SOURCE}"
        "${WEB_TERMINAL_PROTO_SOURCE}"
        "${EDGENODE_NANOPB_PROTO_SOURCE}"
        protobuf::protoc
    COMMENT "Generating edgenode C++ Protobuf protocol"
    VERBATIM)

add_library(edgenode-protocol STATIC
    "${EDGENODE_PROTO_CPP}"
    "${EDGENODE_NANOPB_PROTO_CPP}"
    "${WEB_TERMINAL_PROTO_CPP}")
target_include_directories(edgenode-protocol PUBLIC "${EDGENODE_GENERATED_DIR}")
target_link_libraries(edgenode-protocol PUBLIC protobuf::libprotobuf)
if(MSVC)
    target_compile_options(edgenode-protocol PRIVATE /utf-8 /Zc:__cplusplus)
endif()

# Ruvia's standalone manifest features do not belong to this parent manifest.
unset(VCPKG_MANIFEST_FEATURES CACHE)

find_package(spdlog CONFIG REQUIRED)

add_executable(iot-engine
    service/server.cpp
    service/common/packet-log.cpp
    service/features/gb28181/device/DeviceRegistry.cpp
    service/features/gb28181/media/StreamRegistry.cpp
    service/features/gb28181/media/ZlmSdk.cpp
    service/features/gb28181/runtime.cpp
    service/features/gb28181/sip/DigestAuth.cpp
    service/features/gb28181/sip/SipMessage.cpp
    service/features/gb28181/sip/SipServer.cpp)
target_include_directories(iot-engine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/service/features/gb28181"
    "${EDGENODE_GENERATED_DIR}")
target_link_libraries(iot-engine PRIVATE
    ruvia::web
    pugixml::pugixml
    spdlog::spdlog
    edgenode-protocol
    mk_api
    OpenSSL::Crypto)
if(MSVC)
    target_compile_definitions(iot-engine PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        MediaKitApi_STATIC)
    target_compile_options(iot-engine PRIVATE /bigobj /FS /utf-8 /Zc:__cplusplus)
endif()

enable_testing()
add_executable(collector-protocol-test
    tests/collector-protocol-test.cpp
    service/common/packet-log.cpp)
target_include_directories(collector-protocol-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(collector-protocol-test PRIVATE ruvia::web spdlog::spdlog)
if(MSVC)
    target_compile_options(collector-protocol-test PRIVATE /bigobj /FS /utf-8 /Zc:__cplusplus)
endif()
add_test(NAME collector-protocol COMMAND collector-protocol-test)

add_executable(access-test tests/access-test.cpp)
target_include_directories(access-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(access-test PRIVATE ruvia::web)
if(MSVC)
    target_compile_options(access-test PRIVATE /bigobj /FS /utf-8 /Zc:__cplusplus)
endif()
add_test(NAME access COMMAND access-test)

add_executable(edge-protocol-test tests/edge-protocol-test.cpp)
target_include_directories(edge-protocol-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(edge-protocol-test PRIVATE edgenode-protocol)
if(MSVC)
    target_compile_options(edge-protocol-test PRIVATE /utf-8 /Zc:__cplusplus)
endif()
add_test(NAME edge-protocol COMMAND edge-protocol-test)

add_executable(gb28181-media-test
    tests/gb28181-media-test.cpp
    service/features/gb28181/media/ZlmSdk.cpp)
target_include_directories(gb28181-media-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/service/features/gb28181")
target_link_libraries(gb28181-media-test PRIVATE
    ruvia::web
    spdlog::spdlog
    mk_api
    OpenSSL::Crypto)
if(MSVC)
    target_compile_definitions(gb28181-media-test PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        MediaKitApi_STATIC)
    target_compile_options(gb28181-media-test PRIVATE /utf-8 /Zc:__cplusplus)
endif()
add_test(NAME gb28181-media COMMAND gb28181-media-test)

add_executable(gb28181-sip-test
    tests/gb28181-sip-test.cpp
    service/features/gb28181/device/DeviceRegistry.cpp
    service/features/gb28181/media/ZlmSdk.cpp
    service/features/gb28181/sip/DigestAuth.cpp
    service/features/gb28181/sip/SipMessage.cpp
    service/features/gb28181/sip/SipServer.cpp)
target_include_directories(gb28181-sip-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/service/features/gb28181")
target_link_libraries(gb28181-sip-test PRIVATE
    ruvia::web
    pugixml::pugixml
    spdlog::spdlog
    mk_api
    OpenSSL::Crypto)
if(MSVC)
    target_compile_definitions(gb28181-sip-test PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        MediaKitApi_STATIC)
    target_compile_options(gb28181-sip-test PRIVATE /utf-8 /Zc:__cplusplus)
endif()
add_test(NAME gb28181-sip COMMAND gb28181-sip-test)

find_program(BUN_EXECUTABLE NAMES bun REQUIRED)

add_custom_target(iot-engine-web ALL
    COMMAND "${BUN_EXECUTABLE}" run build:web
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building frontend into ${CMAKE_BINARY_DIR}/web"
    VERBATIM)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.env")
    add_custom_target(iot-engine-env ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/.env"
                "${CMAKE_BINARY_DIR}/.env"
        COMMENT "Copying .env into ${CMAKE_BINARY_DIR}"
        VERBATIM)
endif()

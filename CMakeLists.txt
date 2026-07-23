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

project(iot_engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)

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

find_package(Protobuf CONFIG REQUIRED)

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

# Ruvia's standalone manifest features do not belong to this parent manifest.
unset(VCPKG_MANIFEST_FEATURES CACHE)

find_package(spdlog CONFIG REQUIRED)

add_executable(iot-engine
    service/server.cpp
    service/common/packet-log.cpp
    "${EDGENODE_PROTO_CPP}"
    "${EDGENODE_NANOPB_PROTO_CPP}"
    "${WEB_TERMINAL_PROTO_CPP}")
target_include_directories(iot-engine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${EDGENODE_GENERATED_DIR}")
target_link_libraries(iot-engine PRIVATE
    ruvia::web
    spdlog::spdlog
    protobuf::libprotobuf)
if(MSVC)
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

add_executable(edge-protocol-test
    tests/edge-protocol-test.cpp
    "${EDGENODE_PROTO_CPP}"
    "${EDGENODE_NANOPB_PROTO_CPP}"
    "${WEB_TERMINAL_PROTO_CPP}")
target_include_directories(edge-protocol-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${EDGENODE_GENERATED_DIR}")
target_link_libraries(edge-protocol-test PRIVATE protobuf::libprotobuf)
if(MSVC)
    target_compile_options(edge-protocol-test PRIVATE /utf-8 /Zc:__cplusplus)
endif()
add_test(NAME edge-protocol COMMAND edge-protocol-test)

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

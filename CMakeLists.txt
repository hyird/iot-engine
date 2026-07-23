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
    # Only the three runtime C files are consumed below. Do not configure nanopb's
    # generator or full build in the parent iot-engine project.
    SOURCE_SUBDIR _runtime_sources_only)
FetchContent_MakeAvailable(nanopb)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import google.protobuf, grpc_tools"
    RESULT_VARIABLE EDGENODE_PROTOBUF_PYTHON_RESULT
    OUTPUT_QUIET
    ERROR_QUIET)
if(NOT EDGENODE_PROTOBUF_PYTHON_RESULT EQUAL 0)
    message(FATAL_ERROR
        "edgenode protobuf generation requires Python packages protobuf and grpcio-tools")
endif()

set(EDGENODE_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/edgenode/openwrt/proto")
set(EDGENODE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/edgenode")
set(EDGENODE_PROTO_SOURCE "${EDGENODE_PROTO_DIR}/edge.proto")
set(EDGENODE_PROTO_C "${EDGENODE_GENERATED_DIR}/edge.pb.c")
set(EDGENODE_PROTO_H "${EDGENODE_GENERATED_DIR}/edge.pb.h")
add_custom_command(
    OUTPUT "${EDGENODE_PROTO_C}" "${EDGENODE_PROTO_H}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EDGENODE_GENERATED_DIR}"
    COMMAND "${Python3_EXECUTABLE}"
            "${nanopb_SOURCE_DIR}/generator/nanopb_generator.py"
            -I "${EDGENODE_PROTO_DIR}"
            -I "${nanopb_SOURCE_DIR}/generator/proto"
            -D "${EDGENODE_GENERATED_DIR}"
            "${EDGENODE_PROTO_SOURCE}"
    DEPENDS
        "${EDGENODE_PROTO_SOURCE}"
        "${nanopb_SOURCE_DIR}/generator/nanopb_generator.py"
    COMMENT "Generating edgenode nanopb protocol"
    VERBATIM)

# Ruvia's standalone manifest features do not belong to this parent manifest.
unset(VCPKG_MANIFEST_FEATURES CACHE)

find_package(spdlog CONFIG REQUIRED)

add_executable(iot-engine
    service/server.cpp
    service/common/packet_log.cpp
    "${EDGENODE_PROTO_C}"
    ${nanopb_SOURCE_DIR}/pb_common.c
    ${nanopb_SOURCE_DIR}/pb_encode.c
    ${nanopb_SOURCE_DIR}/pb_decode.c)
target_include_directories(iot-engine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${EDGENODE_GENERATED_DIR}"
    "${nanopb_SOURCE_DIR}")
target_link_libraries(iot-engine PRIVATE ruvia::web spdlog::spdlog)
if(MSVC)
    target_compile_options(iot-engine PRIVATE /bigobj /FS /utf-8 /Zc:__cplusplus)
endif()

enable_testing()
add_executable(southbridge-protocol-test
    tests/southbridge_protocol_test.cpp
    service/common/packet_log.cpp)
target_include_directories(southbridge-protocol-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(southbridge-protocol-test PRIVATE ruvia::web spdlog::spdlog)
if(MSVC)
    target_compile_options(southbridge-protocol-test PRIVATE /bigobj /FS /utf-8 /Zc:__cplusplus)
endif()
add_test(NAME southbridge-protocol COMMAND southbridge-protocol-test)

add_executable(open-access-test tests/open_access_test.cpp)
target_include_directories(open-access-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(open-access-test PRIVATE ruvia::web)
if(MSVC)
    target_compile_options(open-access-test PRIVATE /bigobj /FS /utf-8 /Zc:__cplusplus)
endif()
add_test(NAME open-access COMMAND open-access-test)

add_executable(edge-protocol-test
    tests/edge_protocol_test.c
    edgenode/openwrt/src/edge_protocol.c
    "${EDGENODE_PROTO_C}"
    ${nanopb_SOURCE_DIR}/pb_common.c
    ${nanopb_SOURCE_DIR}/pb_encode.c
    ${nanopb_SOURCE_DIR}/pb_decode.c)
target_include_directories(edge-protocol-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/edgenode/openwrt/src"
    "${EDGENODE_GENERATED_DIR}"
    "${nanopb_SOURCE_DIR}")
if(MSVC)
    target_compile_options(edge-protocol-test PRIVATE /utf-8)
else()
    target_compile_options(edge-protocol-test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME edge-protocol COMMAND edge-protocol-test)

add_executable(edge-runtime-test
    tests/edge_runtime_test.c
    edgenode/openwrt/src/edge_device_runtime.c
    edgenode/openwrt/src/edge_modbus.c
    edgenode/openwrt/src/edge_s7.c)
target_include_directories(edge-runtime-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/edgenode/openwrt/src")
if(MSVC)
    target_compile_options(edge-runtime-test PRIVATE /utf-8 /W4)
else()
    target_compile_options(edge-runtime-test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME edge-runtime COMMAND edge-runtime-test)

add_executable(edge-config-test
    tests/edge_config_test.c
    edgenode/openwrt/src/edge_runtime_config.c
    "${EDGENODE_PROTO_C}"
    ${nanopb_SOURCE_DIR}/pb_common.c
    ${nanopb_SOURCE_DIR}/pb_encode.c
    ${nanopb_SOURCE_DIR}/pb_decode.c)
target_include_directories(edge-config-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/edgenode/openwrt/src"
    "${EDGENODE_GENERATED_DIR}"
    "${nanopb_SOURCE_DIR}")
if(MSVC)
    target_compile_options(edge-config-test PRIVATE /utf-8 /W4)
else()
    target_compile_options(edge-config-test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME edge-config COMMAND edge-config-test)

if(UNIX)
    add_library(edge-acquisition-compile STATIC
        edgenode/openwrt/src/edge_acquisition.c
        edgenode/openwrt/src/edge_runtime_config.c
        edgenode/openwrt/src/edge_device_runtime.c
        edgenode/openwrt/src/edge_modbus.c
        edgenode/openwrt/src/edge_s7.c
        edgenode/openwrt/src/edge_protocol.c
        "${EDGENODE_PROTO_C}"
        ${nanopb_SOURCE_DIR}/pb_common.c
        ${nanopb_SOURCE_DIR}/pb_encode.c
        ${nanopb_SOURCE_DIR}/pb_decode.c)
    target_include_directories(edge-acquisition-compile PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/edgenode/openwrt/src"
        "${EDGENODE_GENERATED_DIR}"
        "${nanopb_SOURCE_DIR}")
    target_compile_options(edge-acquisition-compile PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(edge-acquisition-compile PRIVATE m)
endif()

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
else()
    message(WARNING ".env not found; build/.env will not be created")
endif()

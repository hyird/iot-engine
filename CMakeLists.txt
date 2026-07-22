cmake_minimum_required(VERSION 3.24)

if(NOT CMAKE_TOOLCHAIN_FILE)
    if("$ENV{VCPKG_ROOT}" STREQUAL "")
        message(FATAL_ERROR "VCPKG_ROOT is not set")
    endif()
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VCPKG_ROOT_PATH)
    set(CMAKE_TOOLCHAIN_FILE
        "${VCPKG_ROOT_PATH}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
endif()

if(NOT VCPKG_TARGET_TRIPLET AND NOT "$ENV{VCPKG_DEFAULT_TRIPLET}" STREQUAL "")
    set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "")
endif()

if(NOT VCPKG_HOST_TRIPLET AND NOT "$ENV{VCPKG_DEFAULT_HOST_TRIPLET}" STREQUAL "")
    set(VCPKG_HOST_TRIPLET "$ENV{VCPKG_DEFAULT_HOST_TRIPLET}" CACHE STRING "")
endif()

project(iot_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(RUVIA_ENABLE_POSTGRESQL ON CACHE BOOL "" FORCE)
set(RUVIA_ENABLE_REDIS ON CACHE BOOL "" FORCE)
set(RUVIA_ENABLE_JWT ON CACHE BOOL "" FORCE)

FetchContent_Declare(ruvia
    GIT_REPOSITORY https://github.com/hyird/Ruvia.git
    # Ruvia v0.1.6 includes the MinGW PostgreSQL link-group fix; pin the tag commit.
    GIT_TAG ec7688c3874c858dd7912db46200a67f167802fe)
FetchContent_MakeAvailable(ruvia)

# Ruvia's standalone manifest features do not belong to this parent manifest.
unset(VCPKG_MANIFEST_FEATURES CACHE)

find_package(spdlog CONFIG REQUIRED)

add_executable(iot-engine service/server.cpp service/common/packet_log.cpp)
target_include_directories(iot-engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(iot-engine PRIVATE ruvia::web spdlog::spdlog)

enable_testing()
add_executable(southbridge-protocol-test
    tests/southbridge_protocol_test.cpp
    service/common/packet_log.cpp)
target_include_directories(southbridge-protocol-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(southbridge-protocol-test PRIVATE ruvia::web spdlog::spdlog)
add_test(NAME southbridge-protocol COMMAND southbridge-protocol-test)

add_executable(open-access-test tests/open_access_test.cpp)
target_include_directories(open-access-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(open-access-test PRIVATE ruvia::web)
add_test(NAME open-access COMMAND open-access-test)

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

cmake_minimum_required(VERSION 3.24)

if(NOT CMAKE_TOOLCHAIN_FILE)
    if("$ENV{VCPKG_ROOT}" STREQUAL "")
        message(FATAL_ERROR "VCPKG_ROOT is not set")
    endif()
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VCPKG_ROOT_PATH)
    set(CMAKE_TOOLCHAIN_FILE
        "${VCPKG_ROOT_PATH}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
endif()

project(iot_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(RUVIA_ENABLE_POSTGRESQL ON CACHE BOOL "" FORCE)
set(RUVIA_ENABLE_REDIS ON CACHE BOOL "" FORCE)
set(RUVIA_ENABLE_JWT ON CACHE BOOL "" FORCE)

FetchContent_Declare(ruvia
    GIT_REPOSITORY https://github.com/hyird/Ruvia.git
    GIT_TAG ae556ade49f305fd20dfefa3a045c95ba9ba5f9c)
FetchContent_MakeAvailable(ruvia)

find_program(BUN_EXECUTABLE NAMES bun bun.exe REQUIRED)
add_executable(iot-engine service/server.cpp)
target_include_directories(iot-engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(iot-engine PRIVATE ruvia::web)
add_dependencies(iot-engine)

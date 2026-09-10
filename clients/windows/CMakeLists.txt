cmake_minimum_required(VERSION 3.25)

project(iot_egine_windows NONE)

if(NOT WIN32)
  message(FATAL_ERROR "The Windows client must be configured on Windows.")
endif()

set(IOT_EGINE_WINDOWS_VERSION "0.7.3" CACHE STRING "Windows client version")
set(IOT_EGINE_WINDOWS_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
get_filename_component(IOT_EGINE_WINDOWS_REPOSITORY_ROOT
  "${IOT_EGINE_WINDOWS_REPOSITORY_ROOT}" ABSOLUTE)
set(IOT_EGINE_WINDOWS_BUILD_ROOT "${IOT_EGINE_WINDOWS_REPOSITORY_ROOT}/build")
set(IOT_EGINE_WINDOWS_NATIVE_BUILD "${CMAKE_BINARY_DIR}")
set(IOT_EGINE_WINDOWS_NATIVE_OUTPUT
  "${IOT_EGINE_WINDOWS_BUILD_ROOT}/windows-vpn-native/output")
set(IOT_EGINE_WINDOWS_WINUI_OUTPUT
  "${IOT_EGINE_WINDOWS_BUILD_ROOT}/windows-winui/Release")
set(IOT_EGINE_WINDOWS_PACKAGE_OUTPUT
  "${IOT_EGINE_WINDOWS_BUILD_ROOT}/iot-egine-package")
set(IOT_EGINE_WINDOWS_INSTALLER_OUTPUT
  "${IOT_EGINE_WINDOWS_BUILD_ROOT}/iot-egine-Setup-${IOT_EGINE_WINDOWS_VERSION}-x64.exe")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")

enable_testing()
add_subdirectory(native "${CMAKE_BINARY_DIR}/native")

set(_wireguard_files
  "${IOT_EGINE_WINDOWS_NATIVE_OUTPUT}/tunnel.dll"
  "${IOT_EGINE_WINDOWS_NATIVE_OUTPUT}/wireguard.dll")
add_custom_command(
  OUTPUT ${_wireguard_files}
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_ROOT=${IOT_EGINE_WINDOWS_BUILD_ROOT}"
    "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DLOCK_FILE=${CMAKE_CURRENT_SOURCE_DIR}/wireguard.lock.json"
    "-P" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildWireGuard.cmake"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildWireGuard.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/wireguard.lock.json"
  VERBATIM)
add_custom_target(windows-wireguard DEPENDS ${_wireguard_files})

set(_winui_output "${IOT_EGINE_WINDOWS_WINUI_OUTPUT}/iot-egine.exe")
add_custom_command(
  OUTPUT "${_winui_output}"
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_ROOT=${IOT_EGINE_WINDOWS_BUILD_ROOT}"
    "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DLOCK_FILE=${CMAKE_CURRENT_SOURCE_DIR}/winui/packages.lock.json"
    "-P" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildWinUI.cmake"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildWinUI.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/winui/packages.lock.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/winui/iot-egine.vcxproj"
    "${CMAKE_CURRENT_SOURCE_DIR}/winui/app.manifest"
    "${CMAKE_CURRENT_SOURCE_DIR}/winui/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/common/common.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/common/common.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/assets/product.rc"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/assets/product.ico"
  VERBATIM)
add_custom_target(windows-winui DEPENDS "${_winui_output}")

add_custom_target(windows-native-runtime
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/$<CONFIG>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${IOT_EGINE_WINDOWS_NATIVE_OUTPUT}/tunnel.dll"
    "${CMAKE_BINARY_DIR}/$<CONFIG>/tunnel.dll"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${IOT_EGINE_WINDOWS_NATIVE_OUTPUT}/wireguard.dll"
    "${CMAKE_BINARY_DIR}/$<CONFIG>/wireguard.dll"
  DEPENDS windows-wireguard vpn_service vpn_service_control
  VERBATIM)

add_custom_target(windows-native-tests
  COMMAND "${CMAKE_CTEST_COMMAND}"
    --test-dir "${CMAKE_BINARY_DIR}"
    -C "$<CONFIG>"
    --output-on-failure
  DEPENDS
    windows-native-runtime
    installer_snapshot_tests
    vpn_transport_probe
    vpn_credential_tests
    vpn_service_tests
    vpn_common_tests
    vpn_gui_tests
  VERBATIM)

add_custom_command(
  OUTPUT "${IOT_EGINE_WINDOWS_PACKAGE_OUTPUT}/manifest.json"
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_ROOT=${IOT_EGINE_WINDOWS_BUILD_ROOT}"
    "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DNATIVE_BUILD_DIRECTORY=${IOT_EGINE_WINDOWS_NATIVE_BUILD}"
    "-DNATIVE_OUTPUT=${IOT_EGINE_WINDOWS_NATIVE_OUTPUT}"
    "-DWINUI_OUTPUT=${IOT_EGINE_WINDOWS_WINUI_OUTPUT}"
    "-DPACKAGE_DIRECTORY=${IOT_EGINE_WINDOWS_PACKAGE_OUTPUT}"
    "-DCLIENT_CONFIG=$<CONFIG>"
    "-DVERSION=${IOT_EGINE_WINDOWS_VERSION}"
    "-DLOCK_FILE=${CMAKE_CURRENT_SOURCE_DIR}/wireguard.lock.json"
    "-DWINUI_LOCK_FILE=${CMAKE_CURRENT_SOURCE_DIR}/winui/packages.lock.json"
    "-P" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageWindowsClient.cmake"
  DEPENDS
    windows-wireguard
    windows-winui
    windows-native-tests
    ${_wireguard_files}
    ${_winui_output}
    vpn_service
    vpn_service_control
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageWindowsClient.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/wireguard.lock.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/winui/packages.lock.json"
  VERBATIM)
add_custom_target(windows-package
  DEPENDS "${IOT_EGINE_WINDOWS_PACKAGE_OUTPUT}/manifest.json")

add_custom_command(
  OUTPUT "${IOT_EGINE_WINDOWS_INSTALLER_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_ROOT=${IOT_EGINE_WINDOWS_BUILD_ROOT}"
    "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DPACKAGE_DIRECTORY=${IOT_EGINE_WINDOWS_PACKAGE_OUTPUT}"
    "-DOUTPUT_FILE=${IOT_EGINE_WINDOWS_INSTALLER_OUTPUT}"
    "-DVERSION=${IOT_EGINE_WINDOWS_VERSION}"
    "-DSKIP_PACKAGE_BUILD=ON"
    "-P" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildWindowsInstaller.cmake"
  DEPENDS
    windows-package
    "${IOT_EGINE_WINDOWS_PACKAGE_OUTPUT}/manifest.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildWindowsInstaller.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/installer/Package.wxs"
    "${CMAKE_CURRENT_SOURCE_DIR}/installer/Bundle.wxs"
  VERBATIM)
add_custom_target(windows-installer
  DEPENDS "${IOT_EGINE_WINDOWS_INSTALLER_OUTPUT}")

add_custom_target(windows-installer-from-package
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_ROOT=${IOT_EGINE_WINDOWS_BUILD_ROOT}"
    "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DPACKAGE_DIRECTORY=${IOT_EGINE_WINDOWS_PACKAGE_OUTPUT}"
    "-DOUTPUT_FILE=${IOT_EGINE_WINDOWS_INSTALLER_OUTPUT}"
    "-DVERSION=${IOT_EGINE_WINDOWS_VERSION}"
    "-DSKIP_PACKAGE_BUILD=ON"
    "-P" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildWindowsInstaller.cmake"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildWindowsInstaller.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/installer/Package.wxs"
    "${CMAKE_CURRENT_SOURCE_DIR}/installer/Bundle.wxs"
  VERBATIM)

add_custom_target(windows-client ALL DEPENDS windows-package)

message(STATUS "Windows client targets: windows-client, windows-package, windows-installer, windows-installer-from-package")

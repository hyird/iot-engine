cmake_minimum_required(VERSION 3.25)

if(NOT WIN32)
  message(FATAL_ERROR "Windows installer packaging requires Windows.")
endif()

foreach(_required IN ITEMS BUILD_ROOT SOURCE_ROOT PACKAGE_DIRECTORY OUTPUT_FILE VERSION)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "${_required} is required.")
  endif()
endforeach()
if(NOT DEFINED SKIP_PACKAGE_BUILD)
  set(SKIP_PACKAGE_BUILD OFF)
endif()

function(run_checked)
  execute_process(
    COMMAND ${ARGV}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(_stdout)
    message(STATUS "${_stdout}")
  endif()
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Command failed (${_result}): ${ARGV}\n${_stderr}")
  endif()
endfunction()

function(json_value output json)
  set(_path ${ARGN})
  string(JSON _value GET "${json}" ${_path})
  set(${output} "${_value}" PARENT_SCOPE)
endfunction()

function(xml_escape output value)
  set(_value "${value}")
  string(REPLACE "&" "&amp;" _value "${_value}")
  string(REPLACE "<" "&lt;" _value "${_value}")
  string(REPLACE ">" "&gt;" _value "${_value}")
  string(REPLACE "\"" "&quot;" _value "${_value}")
  string(REPLACE "'" "&apos;" _value "${_value}")
  set(${output} "${_value}" PARENT_SCOPE)
endfunction()

function(stable_id output value)
  string(SHA256 _digest "${value}")
  string(SUBSTRING "${_digest}" 0 30 _short)
  set(${output} "X${_short}" PARENT_SCOPE)
endfunction()

get_filename_component(_package "${PACKAGE_DIRECTORY}" ABSOLUTE)
if(NOT EXISTS "${_package}/manifest.json")
  if(SKIP_PACKAGE_BUILD)
    message(FATAL_ERROR "Package manifest is missing: ${_package}/manifest.json")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DBUILD_ROOT=${BUILD_ROOT}"
      "-DSOURCE_ROOT=${SOURCE_ROOT}"
      "-DNATIVE_BUILD_DIRECTORY=${BUILD_ROOT}/windows-client"
      "-DNATIVE_OUTPUT=${BUILD_ROOT}/windows-vpn-native/output"
      "-DWINUI_OUTPUT=${BUILD_ROOT}/windows-winui/Release"
      "-DPACKAGE_DIRECTORY=${_package}"
      "-DCLIENT_CONFIG=Release"
      "-DVERSION=${VERSION}"
      "-DLOCK_FILE=${SOURCE_ROOT}/wireguard.lock.json"
      "-DWINUI_LOCK_FILE=${SOURCE_ROOT}/winui/packages.lock.json"
      -P "${SOURCE_ROOT}/cmake/PackageWindowsClient.cmake"
    RESULT_VARIABLE _package_result)
  if(NOT _package_result EQUAL 0)
    message(FATAL_ERROR "Windows client package build failed (${_package_result}).")
  endif()
endif()

file(READ "${_package}/manifest.json" _manifest)
json_value(_manifest_version "${_manifest}" version)
if(NOT _manifest_version STREQUAL "${VERSION}")
  message(FATAL_ERROR "Unexpected package version: ${_manifest_version}")
endif()
string(JSON _manifest_file_count LENGTH "${_manifest}" files)
math(EXPR _manifest_last "${_manifest_file_count} - 1")
string(TOLOWER "${_package}" _package_lower)
string(LENGTH "${_package_lower}" _package_length)
if(_manifest_file_count GREATER 0)
  foreach(_index RANGE 0 ${_manifest_last})
    json_value(_relative "${_manifest}" files ${_index} path)
    json_value(_expected_hash "${_manifest}" files ${_index} sha256)
    json_value(_expected_bytes "${_manifest}" files ${_index} bytes)
    get_filename_component(_file "${_package}/${_relative}" ABSOLUTE)
    string(TOLOWER "${_file}" _file_lower)
    string(SUBSTRING "${_file_lower}" 0 ${_package_length} _file_prefix)
    if(NOT _file_prefix STREQUAL "${_package_lower}" OR
       NOT _file_lower MATCHES "^${_package_lower}/")
      message(FATAL_ERROR "Invalid manifest path: ${_relative}")
    endif()
    if(NOT EXISTS "${_file}" OR IS_DIRECTORY "${_file}")
      message(FATAL_ERROR "Manifest file is missing: ${_relative}")
    endif()
    file(SHA256 "${_file}" _actual_hash)
    file(SIZE "${_file}" _actual_bytes)
    string(TOLOWER "${_actual_hash}" _actual_hash)
    if(NOT _actual_hash STREQUAL "${_expected_hash}" OR
       NOT _actual_bytes STREQUAL "${_expected_bytes}")
      message(FATAL_ERROR "Payload checksum mismatch: ${_relative}")
    endif()
  endforeach()
endif()
file(GLOB_RECURSE _package_files LIST_DIRECTORIES false "${_package}/*")
list(LENGTH _package_files _actual_file_count)
math(EXPR _expected_file_count "${_manifest_file_count} + 1")
if(NOT _actual_file_count EQUAL _expected_file_count)
  message(FATAL_ERROR "Unexpected payload file count: ${_actual_file_count}; expected ${_expected_file_count}.")
endif()

set(_work "${BUILD_ROOT}/windows-wix")
file(MAKE_DIRECTORY "${_work}")
include("${CMAKE_CURRENT_LIST_DIR}/PrepareDotnet.cmake")
iot_prepare_dotnet("${BUILD_ROOT}")
set(_wix_dir "${BUILD_ROOT}/windows-wix-tools")
set(_wix "${_wix_dir}/wix.exe")
if(NOT EXISTS "${_wix}")
  if(NOT EXISTS "${_dotnet}")
    message(FATAL_ERROR "The pinned .NET SDK is missing: ${_dotnet}")
  endif()
  file(MAKE_DIRECTORY "${_wix_dir}")
  run_checked("${_dotnet}" tool install wix --version 6.0.2 --tool-path "${_wix_dir}")
endif()
run_checked("${_wix}" extension add -g WixToolset.BootstrapperApplications.wixext/6.0.2)

set(_payload "${_work}/Payload.wxs")
set(_xml "<Wix xmlns=\"http://wixtoolset.org/schemas/v4/wxs\"><Fragment>\n")
set(_directories "")
file(GLOB_RECURSE _files LIST_DIRECTORIES false "${_package}/*")
list(SORT _files)
foreach(_file IN LISTS _files)
  file(RELATIVE_PATH _relative "${_package}" "${_file}")
  string(REPLACE "\\" "/" _relative "${_relative}")
  get_filename_component(_parent "${_relative}" DIRECTORY)
  if(_parent STREQUAL "")
    set(_current_parent "INSTALLFOLDER")
  else()
    set(_current_parent "INSTALLFOLDER")
    string(REPLACE "/" ";" _parts "${_parent}")
    set(_path_so_far "")
    foreach(_part IN LISTS _parts)
      if(_path_so_far STREQUAL "")
        set(_path_so_far "${_part}")
      else()
        set(_path_so_far "${_path_so_far}/${_part}")
      endif()
      stable_id(_directory_id "dir/${_path_so_far}")
      xml_escape(_directory_name "${_part}")
      list(FIND _directories "${_path_so_far}" _directory_index)
      if(_directory_index EQUAL -1)
        string(APPEND _xml "<DirectoryRef Id=\"${_current_parent}\"><Directory Id=\"${_directory_id}\" Name=\"${_directory_name}\"/></DirectoryRef>\n")
        list(APPEND _directories "${_path_so_far}")
      endif()
      set(_current_parent "${_directory_id}")
    endforeach()
  endif()
  stable_id(_component_id "component/${_relative}")
  if(_relative STREQUAL "Gui/iot-egine.exe")
    set(_file_id "AppExe")
  else()
    stable_id(_file_id "file/${_relative}")
  endif()
  xml_escape(_source "${_file}")
  string(APPEND _xml "<DirectoryRef Id=\"${_current_parent}\"><Component Id=\"${_component_id}\" Guid=\"*\" Bitness=\"always64\"><File Id=\"${_file_id}\" Source=\"${_source}\" KeyPath=\"yes\"/></Component></DirectoryRef>\n")
endforeach()
string(APPEND _xml "<ComponentGroup Id=\"Payload\">\n")
foreach(_file IN LISTS _files)
  file(RELATIVE_PATH _relative "${_package}" "${_file}")
  string(REPLACE "\\" "/" _relative "${_relative}")
  stable_id(_component_id "component/${_relative}")
  string(APPEND _xml "<ComponentRef Id=\"${_component_id}\"/>\n")
endforeach()
string(APPEND _xml "</ComponentGroup></Fragment></Wix>\n")
file(WRITE "${_payload}" "${_xml}")

set(_msi "${_work}/iot-egine-${VERSION}-x64.msi")
run_checked("${_wix}" build -arch x64
  -d "Version=${VERSION}"
  -d "PackageDir=${_package}"
  -d "SourceRoot=${SOURCE_ROOT}"
  "${SOURCE_ROOT}/installer/Package.wxs"
  "${_payload}"
  -o "${_msi}")
set(_output "${OUTPUT_FILE}")
get_filename_component(_output_directory "${_output}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_directory}")
run_checked("${_wix}" build -arch x64
  -ext WixToolset.BootstrapperApplications.wixext
  -d "Version=${VERSION}"
  -d "MsiPath=${_msi}"
  -d "SourceRoot=${SOURCE_ROOT}"
  "${SOURCE_ROOT}/installer/Bundle.wxs"
  -o "${_output}")
if(NOT EXISTS "${_output}")
  message(FATAL_ERROR "WiX did not produce the installer: ${_output}")
endif()
file(SHA256 "${_output}" _installer_hash)
message(STATUS "Installer: ${_output}")
message(STATUS "Installer SHA256: ${_installer_hash}")

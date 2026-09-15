cmake_minimum_required(VERSION 3.25)

# This installs and removes the real service. Only run on an isolated CI worker.
if(NOT WIN32 OR NOT "$ENV{CI}" STREQUAL "true" OR "$ENV{RUNNER_TEMP}" STREQUAL "")
  message(FATAL_ERROR "Installer lifecycle testing requires an isolated Windows CI worker.")
endif()
foreach(_required IN ITEMS BUILD_ROOT SOURCE_ROOT PACKAGE_DIRECTORY OUTPUT_FILE VERSION)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "${_required} is required.")
  endif()
endforeach()
execute_process(COMMAND sc.exe query iot-egine.tunnel RESULT_VARIABLE _existing
  OUTPUT_QUIET ERROR_QUIET)
if(NOT _existing EQUAL 1060)
  message(FATAL_ERROR "Installer lifecycle test requires no existing client service.")
endif()
set(_test_root "${BUILD_ROOT}/windows-installer-lifecycle")
file(MAKE_DIRECTORY "${_test_root}")
set(_first "${_test_root}/first.exe")
set(_second "${_test_root}/second.exe")
file(COPY_FILE "${OUTPUT_FILE}" "${_first}")
# Rebuild with a distinct MSI ProductCode and Burn BundleId at the same version.
execute_process(COMMAND "${CMAKE_COMMAND}"
  "-DBUILD_ROOT=${BUILD_ROOT}" "-DSOURCE_ROOT=${SOURCE_ROOT}"
  "-DPACKAGE_DIRECTORY=${PACKAGE_DIRECTORY}" "-DOUTPUT_FILE=${_second}"
  "-DVERSION=${VERSION}" -DSKIP_PACKAGE_BUILD=ON
  -P "${SOURCE_ROOT}/cmake/BuildWindowsInstaller.cmake"
  COMMAND_ERROR_IS_FATAL ANY)

function(run_installer file action label)
  execute_process(COMMAND "${file}" "${action}" /quiet /norestart /log "${_test_root}/${label}.log"
    RESULT_VARIABLE _result TIMEOUT 180)
  if(NOT _result EQUAL 0 AND NOT _result EQUAL 3010)
    message(FATAL_ERROR "${label} failed (${_result}); see ${_test_root}/${label}.log")
  endif()
endfunction()
function(assert_service_running)
  execute_process(COMMAND powershell.exe -NoProfile -NonInteractive -Command
    "$s = Get-Service -Name 'iot-egine.tunnel' -ErrorAction Stop; if ($s.Status -ne 'Running' -or $s.StartType -ne 'Automatic') { exit 1 }"
    RESULT_VARIABLE _result)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "The installed client service must exist, run and start automatically.")
  endif()
endfunction()
function(assert_installed_payload)
  file(READ "$ENV{ProgramW6432}/iot-egine/manifest.json" _manifest)
  string(JSON _count LENGTH "${_manifest}" files)
  math(EXPR _last "${_count} - 1")
  foreach(_index RANGE 0 ${_last})
    string(JSON _relative GET "${_manifest}" files ${_index} path)
    string(JSON _expected GET "${_manifest}" files ${_index} sha256)
    file(SHA256 "$ENV{ProgramW6432}/iot-egine/${_relative}" _actual)
    if(NOT _actual STREQUAL _expected)
      message(FATAL_ERROR "Installed payload mismatch: ${_relative}")
    endif()
  endforeach()
endfunction()
run_installer("${_first}" /install fresh-install)
assert_service_running()
assert_installed_payload()
# Keep the PE version unchanged, but make its bytes differ from the new payload.
file(APPEND "$ENV{ProgramW6432}/iot-egine/Service/IotVpn.ServiceControl.exe" "same-version-check")
run_installer("${_second}" /install same-version-upgrade)
assert_service_running()
assert_installed_payload()
file(APPEND "$ENV{ProgramW6432}/iot-egine/Service/IotVpn.ServiceControl.exe" "repair-check")
run_installer("${_second}" /repair repair)
assert_service_running()
assert_installed_payload()
run_installer("${_second}" /uninstall uninstall)
execute_process(COMMAND sc.exe query iot-egine.tunnel RESULT_VARIABLE _remaining
  OUTPUT_QUIET ERROR_QUIET)
if(NOT _remaining EQUAL 1060)
  message(FATAL_ERROR "Uninstall did not remove the client service.")
endif()
message(STATUS "PASS fresh installation, same-version upgrade, repair and uninstall")

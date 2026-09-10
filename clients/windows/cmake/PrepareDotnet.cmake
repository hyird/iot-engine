cmake_minimum_required(VERSION 3.25)

function(iot_prepare_dotnet build_root)
  if(DEFINED DOTNET_EXECUTABLE AND NOT DOTNET_EXECUTABLE STREQUAL "")
    get_filename_component(_candidate "${DOTNET_EXECUTABLE}" ABSOLUTE)
  else()
    find_program(_candidate NAMES dotnet.exe dotnet
      HINTS
        "${build_root}/windows-vpn-tools/dotnet"
        "$ENV{DOTNET_ROOT}"
        "$ENV{ProgramW6432}/dotnet"
        "$ENV{ProgramFiles}/dotnet"
      NO_CACHE)
  endif()
  if(NOT _candidate OR NOT EXISTS "${_candidate}" OR IS_DIRECTORY "${_candidate}")
    message(FATAL_ERROR
      "Install the .NET 8 SDK, or configure DOTNET_EXECUTABLE with its dotnet.exe path.")
  endif()
  file(REAL_PATH "${_candidate}" _candidate)
  get_filename_component(_runtime_root "${_candidate}" DIRECTORY)
  set(ENV{DOTNET_ROOT} "${_runtime_root}")
  execute_process(COMMAND "${_candidate}" --list-sdks
    RESULT_VARIABLE _sdk_result OUTPUT_VARIABLE _sdks ERROR_VARIABLE _sdk_error)
  execute_process(COMMAND "${_candidate}" --list-runtimes
    RESULT_VARIABLE _runtime_result OUTPUT_VARIABLE _runtimes ERROR_VARIABLE _runtime_error)
  string(REPLACE "\r\n" "\n" _sdks "${_sdks}")
  string(REPLACE "\r\n" "\n" _runtimes "${_runtimes}")
  if(NOT _sdk_result EQUAL 0 OR
     NOT _sdks MATCHES "(^|\n)([89]|[1-9][0-9]+)\\.[0-9]+\\.[0-9]+ ")
    message(FATAL_ERROR
      "WiX preparation requires a .NET SDK version 8 or later: ${_candidate}\n${_sdk_error}")
  endif()
  if(NOT _runtime_result EQUAL 0 OR
     NOT _runtimes MATCHES "(^|\n)Microsoft\\.NETCore\\.App ([89]|[1-9][0-9]+)\\.[0-9]+\\.[0-9]+ ")
    message(FATAL_ERROR
      "Install a .NET 8 or later SDK and runtime, or select its dotnet.exe with DOTNET_EXECUTABLE.\n${_runtime_error}")
  endif()
  set(_dotnet "${_candidate}" PARENT_SCOPE)
  message(STATUS "Verified .NET SDK and runtime: ${_candidate}")
endfunction()

function(fetch_pinned_git_source source repository revision)
  if(EXISTS "${source}/.git")
    execute_process(
      COMMAND git -C "${source}" status --porcelain --untracked-files=no
      RESULT_VARIABLE _status_result
      OUTPUT_VARIABLE _dirty
      ERROR_VARIABLE _status_error
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT "${_status_result}" STREQUAL "0")
      message(FATAL_ERROR "Cannot inspect cached WireGuard source checkout '${source}': ${_status_error}")
    endif()
    if(_dirty)
      message(FATAL_ERROR "WireGuard source checkout contains local edits: ${_dirty}")
    endif()
  else()
    if(EXISTS "${source}")
      file(GLOB _source_contents LIST_DIRECTORIES true
        "${source}/*" "${source}/.[!.]*" "${source}/..?*")
      if(_source_contents)
        message(FATAL_ERROR "WireGuard source path exists but is not a Git checkout: ${source}")
      endif()
    endif()
    file(MAKE_DIRECTORY "${source}")
    execute_process(
      COMMAND git -C "${source}" init --quiet
      RESULT_VARIABLE _init_result
      OUTPUT_VARIABLE _init_output
      ERROR_VARIABLE _init_error)
    if(NOT "${_init_result}" STREQUAL "0")
      message(FATAL_ERROR "Cannot initialize WireGuard source checkout '${source}' (${_init_result}): ${_init_error}")
    endif()
  endif()

  # Refetch without negotiation to repair caches that retain a commit but omit its tree.
  execute_process(
    COMMAND git -C "${source}" fetch --refetch --no-tags --depth=1 "${repository}" "${revision}"
    RESULT_VARIABLE _fetch_result
    OUTPUT_VARIABLE _fetch_output
    ERROR_VARIABLE _fetch_error)
  if(_fetch_output)
    message(STATUS "${_fetch_output}")
  endif()
  if(NOT "${_fetch_result}" STREQUAL "0")
    message(FATAL_ERROR
      "Failed to fetch pinned WireGuard commit '${revision}' from '${repository}' (${_fetch_result}):\n${_fetch_error}")
  endif()

  foreach(_object_type IN ITEMS commit tree)
    set(_object_spec "${revision}^")
    string(APPEND _object_spec "{${_object_type}}")
    execute_process(
      COMMAND git -C "${source}" cat-file -e "${_object_spec}"
      RESULT_VARIABLE _object_result
      ERROR_VARIABLE _object_error)
    if(NOT "${_object_result}" STREQUAL "0")
      message(FATAL_ERROR
        "Fetched WireGuard revision '${revision}' is missing its ${_object_type} object: ${_object_error}")
    endif()
  endforeach()

  execute_process(
    COMMAND git -C "${source}" checkout --detach "${revision}"
    RESULT_VARIABLE _checkout_result
    OUTPUT_VARIABLE _checkout_output
    ERROR_VARIABLE _checkout_error)
  if(_checkout_output)
    message(STATUS "${_checkout_output}")
  endif()
  if(NOT "${_checkout_result}" STREQUAL "0")
    message(FATAL_ERROR
      "Failed to check out pinned WireGuard commit '${revision}' (${_checkout_result}):\n${_checkout_error}")
  endif()

  execute_process(
    COMMAND git -C "${source}" rev-parse HEAD
    RESULT_VARIABLE _revision_result
    OUTPUT_VARIABLE _actual_revision
    ERROR_VARIABLE _revision_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT "${_revision_result}" STREQUAL "0" OR NOT _actual_revision STREQUAL "${revision}")
    message(FATAL_ERROR "Wrong WireGuard source revision: ${_actual_revision}${_revision_error}")
  endif()
endfunction()

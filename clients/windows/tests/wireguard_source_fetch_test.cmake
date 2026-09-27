cmake_minimum_required(VERSION 3.25)

get_filename_component(_repository_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(TEST_BUILD_ROOT "${_repository_root}/build/windows-wireguard-source-fetch-test")

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/FetchPinnedGit.cmake")

function(run_checked)
  execute_process(
    COMMAND ${ARGV}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(NOT "${_result}" STREQUAL "0")
    message(FATAL_ERROR "Command failed (${_result}): ${ARGV}\n${_stdout}${_stderr}")
  endif()
endfunction()

function(assert_revision source expected)
  execute_process(
    COMMAND git -C "${source}" rev-parse HEAD
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _actual
    ERROR_VARIABLE _error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT "${_result}" STREQUAL "0" OR NOT _actual STREQUAL "${expected}")
    message(FATAL_ERROR "Unexpected fixture checkout '${_actual}', expected '${expected}': ${_error}")
  endif()
  set(_tree_spec "${expected}^")
  string(APPEND _tree_spec "{tree}")
  execute_process(
    COMMAND git -C "${source}" cat-file -e "${_tree_spec}"
    RESULT_VARIABLE _tree_result
    ERROR_VARIABLE _tree_error)
  if(NOT "${_tree_result}" STREQUAL "0")
    message(FATAL_ERROR "Fixture checkout is missing its commit tree: ${_tree_error}")
  endif()
endfunction()

file(REMOVE_RECURSE "${TEST_BUILD_ROOT}")
file(MAKE_DIRECTORY "${TEST_BUILD_ROOT}")
set(ENV{GIT_TERMINAL_PROMPT} 0)
set(ENV{GIT_AUTHOR_NAME} "WireGuard CMake fixture")
set(ENV{GIT_AUTHOR_EMAIL} "wireguard-cmake-fixture@example.invalid")
set(ENV{GIT_COMMITTER_NAME} "WireGuard CMake fixture")
set(ENV{GIT_COMMITTER_EMAIL} "wireguard-cmake-fixture@example.invalid")

set(_seed "${TEST_BUILD_ROOT}/seed")
set(_remote "${TEST_BUILD_ROOT}/remote.git")
set(_source "${TEST_BUILD_ROOT}/source")
set(_incomplete_source "${TEST_BUILD_ROOT}/incomplete-source")
set(_helper "${CMAKE_CURRENT_LIST_DIR}/../cmake/FetchPinnedGit.cmake")

run_checked(git init --quiet "${_seed}")
run_checked(git -C "${_seed}" config user.name "WireGuard CMake fixture")
run_checked(git -C "${_seed}" config user.email "wireguard-cmake-fixture@example.invalid")
file(WRITE "${_seed}/fixture.txt" "pinned revision\n")
run_checked(git -C "${_seed}" add fixture.txt)
run_checked(git -C "${_seed}" commit --quiet -m "pinned fixture revision")
execute_process(
  COMMAND git -C "${_seed}" rev-parse HEAD
  RESULT_VARIABLE _pin_result
  OUTPUT_VARIABLE _pinned_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT "${_pin_result}" STREQUAL "0")
  message(FATAL_ERROR "Cannot read fixture commit: ${_pinned_revision}")
endif()

run_checked(git init --quiet --bare "${_remote}")
run_checked(git -C "${_seed}" remote add origin "${_remote}")
file(WRITE "${_seed}/fixture.txt" "newer cached revision\n")
run_checked(git -C "${_seed}" add fixture.txt)
run_checked(git -C "${_seed}" commit --quiet -m "newer fixture revision")
execute_process(
  COMMAND git -C "${_seed}" rev-parse HEAD
  RESULT_VARIABLE _newer_result
  OUTPUT_VARIABLE _newer_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT "${_newer_result}" STREQUAL "0")
  message(FATAL_ERROR "Cannot read newer fixture commit: ${_newer_revision}")
endif()
run_checked(git -C "${_seed}" push --quiet origin HEAD:refs/heads/main)

# First use creates an empty repository and fetches just the pinned commit.
fetch_pinned_git_source("${_source}" "${_remote}" "${_pinned_revision}")
assert_revision("${_source}" "${_pinned_revision}")
if(NOT EXISTS "${_source}/.git/shallow")
  message(FATAL_ERROR "Fixture fetch did not create the expected shallow checkout.")
endif()

# A restored shallow cache at another revision must be refreshed to the lock.
run_checked(git -C "${_source}" fetch --refetch --no-tags --depth=1 "${_remote}" "${_newer_revision}")
run_checked(git -C "${_source}" checkout --quiet --detach "${_newer_revision}")
assert_revision("${_source}" "${_newer_revision}")
fetch_pinned_git_source("${_source}" "${_remote}" "${_pinned_revision}")
assert_revision("${_source}" "${_pinned_revision}")

# Reproduce an incomplete restored object database: the commit exists but its tree does not.
run_checked(git init --quiet "${_incomplete_source}")
string(SUBSTRING "${_pinned_revision}" 0 2 _commit_object_directory)
string(SUBSTRING "${_pinned_revision}" 2 -1 _commit_object_filename)
set(_seed_commit_object "${_seed}/.git/objects/${_commit_object_directory}/${_commit_object_filename}")
set(_incomplete_commit_object "${_incomplete_source}/.git/objects/${_commit_object_directory}/${_commit_object_filename}")
if(NOT EXISTS "${_seed_commit_object}")
  message(FATAL_ERROR "Seed commit loose object is missing: ${_seed_commit_object}")
endif()
file(MAKE_DIRECTORY "${_incomplete_source}/.git/objects/${_commit_object_directory}")
file(COPY_FILE "${_seed_commit_object}" "${_incomplete_commit_object}")
if(NOT EXISTS "${_incomplete_commit_object}")
  message(FATAL_ERROR "Incomplete cache commit object was not copied: ${_incomplete_commit_object}")
endif()
execute_process(
  COMMAND git -C "${_incomplete_source}" cat-file -e "${_pinned_revision}^{commit}"
  RESULT_VARIABLE _commit_result
  ERROR_VARIABLE _commit_error)
if(NOT "${_commit_result}" STREQUAL "0")
  message(FATAL_ERROR "Cannot read copied fixture commit object: ${_commit_error}")
endif()
set(_pinned_tree_spec "${_pinned_revision}^")
string(APPEND _pinned_tree_spec "{tree}")
execute_process(
  COMMAND git -C "${_incomplete_source}" cat-file -e "${_pinned_tree_spec}"
  RESULT_VARIABLE _missing_tree_result
  ERROR_VARIABLE _missing_tree_error)
if("${_missing_tree_result}" STREQUAL "0")
  message(FATAL_ERROR "Incomplete cache fixture unexpectedly contains the commit tree.")
endif()
fetch_pinned_git_source("${_incomplete_source}" "${_remote}" "${_pinned_revision}")
assert_revision("${_incomplete_source}" "${_pinned_revision}")

# Fetch failures must identify the pinned revision and upstream instead of failing later at checkout.
set(_failure_driver "${TEST_BUILD_ROOT}/expected-fetch-failure.cmake")
file(WRITE "${_failure_driver}" [=[
include("${FETCH_HELPER}")
fetch_pinned_git_source("${SOURCE_PATH}" "${REPOSITORY}" "${REVISION}")
]=])
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-DFETCH_HELPER=${_helper}"
    "-DSOURCE_PATH=${TEST_BUILD_ROOT}/failed-source"
    "-DREPOSITORY=${_remote}"
    "-DREVISION=0000000000000000000000000000000000000000"
    -P "${_failure_driver}"
  RESULT_VARIABLE _failure_result
  OUTPUT_VARIABLE _failure_output
  ERROR_VARIABLE _failure_error)
set(_failure_log "${_failure_output}${_failure_error}")
if("${_failure_result}" STREQUAL "0" OR
   NOT _failure_log MATCHES "Failed to fetch pinned WireGuard commit" OR
   NOT _failure_log MATCHES "0000000000000000000000000000000000000000")
  message(FATAL_ERROR "Fetch failure was not reported clearly:\n${_failure_log}")
endif()

file(REMOVE_RECURSE "${TEST_BUILD_ROOT}")
message(STATUS "WireGuard pinned source fetch fixture passed.")

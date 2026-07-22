if(NOT PATCH_WORKING_DIRECTORY OR NOT PATCH_FILE)
    message(FATAL_ERROR "PATCH_WORKING_DIRECTORY and PATCH_FILE are required")
endif()

find_program(GIT_EXECUTABLE git REQUIRED)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${PATCH_WORKING_DIRECTORY}"
    RESULT_VARIABLE patch_check_result
    ERROR_VARIABLE patch_check_error)

if(patch_check_result EQUAL 0)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${PATCH_FILE}"
        WORKING_DIRECTORY "${PATCH_WORKING_DIRECTORY}"
        RESULT_VARIABLE patch_result
        ERROR_VARIABLE patch_error)
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${PATCH_FILE}: ${patch_error}")
    endif()
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${PATCH_WORKING_DIRECTORY}"
    RESULT_VARIABLE reverse_check_result
    ERROR_VARIABLE reverse_check_error)

if(NOT reverse_check_result EQUAL 0)
    message(FATAL_ERROR
        "Patch ${PATCH_FILE} is neither applicable nor already applied. "
        "Apply check: ${patch_check_error} Reverse check: ${reverse_check_error}")
endif()

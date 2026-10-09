# Idempotent FetchContent patch for the pinned ftpsrv dependency.
# CMake may rerun PATCH_COMMAND against an existing source checkout.
if(NOT DEFINED GIT_EXECUTABLE OR NOT DEFINED PATCH_FILE OR NOT DEFINED DEP_SOURCE)
    message(FATAL_ERROR "Missing Git executable, patch file, or dependency source directory")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace "${PATCH_FILE}"
    WORKING_DIRECTORY "${DEP_SOURCE}"
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET
    ERROR_QUIET
)

if(already_applied EQUAL 0)
    message(STATUS "ftpsrv upload callback patch already applied")
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --check --ignore-whitespace "${PATCH_FILE}"
    WORKING_DIRECTORY "${DEP_SOURCE}"
    RESULT_VARIABLE can_apply
)

if(NOT can_apply EQUAL 0)
    message(FATAL_ERROR "ftpsrv patch neither applies nor matches an already-patched checkout")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${PATCH_FILE}"
    WORKING_DIRECTORY "${DEP_SOURCE}"
    RESULT_VARIABLE applied
)

if(NOT applied EQUAL 0)
    message(FATAL_ERROR "ftpsrv patch failed")
endif()

message(STATUS "Applied ftpsrv upload callback patch")

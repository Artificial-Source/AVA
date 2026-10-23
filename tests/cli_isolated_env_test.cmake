if(NOT DEFINED AVA_CLI_TEST_ROOT)
  message(FATAL_ERROR "AVA_CLI_TEST_ROOT is required")
endif()
if(NOT DEFINED AVA_CLI_ENV_PYTHON)
  message(FATAL_ERROR "AVA_CLI_ENV_PYTHON is required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cli_test_common.cmake")

get_filename_component(TEST_ROOT "${AVA_CLI_TEST_ROOT}" ABSOLUTE)
file(REMOVE_RECURSE "${TEST_ROOT}")
# Exercise a symlinked fixture root even when the host build path is physical.
# The helper's absolute paths must retain this spelling rather than resolving it.
file(MAKE_DIRECTORY "${TEST_ROOT}/physical")
file(CREATE_LINK "${TEST_ROOT}/physical" "${TEST_ROOT}/linked" SYMBOLIC)
set(FIXTURE_ROOT "${TEST_ROOT}/linked")
ava_cli_isolated_env(AVA_CLI_ENV "${FIXTURE_ROOT}")
find_program(AVA_GNU_ENV NAMES env REQUIRED)

set(CHECK_COMMAND
    "${AVA_CLI_ENV_PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/cli_isolated_env_test.py"
    check "${FIXTURE_ROOT}")

execute_process(
  COMMAND "${AVA_GNU_ENV}" ${AVA_CLI_ENV} ${CHECK_COMMAND}
  OUTPUT_VARIABLE GNU_OUTPUT ERROR_VARIABLE GNU_ERROR RESULT_VARIABLE GNU_RESULT)
if(NOT GNU_RESULT EQUAL 0)
  message(FATAL_ERROR
          "GNU env isolated child failed with ${GNU_RESULT}\nstdout:\n${GNU_OUTPUT}\nstderr:\n${GNU_ERROR}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${AVA_CLI_ENV} ${CHECK_COMMAND}
  OUTPUT_VARIABLE CMAKE_OUTPUT ERROR_VARIABLE CMAKE_ERROR RESULT_VARIABLE CMAKE_RESULT)
if(NOT CMAKE_RESULT EQUAL 0)
  message(FATAL_ERROR
          "cmake -E env isolated child failed with ${CMAKE_RESULT}\nstdout:\n${CMAKE_OUTPUT}\nstderr:\n${CMAKE_ERROR}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")

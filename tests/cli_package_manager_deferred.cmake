if(NOT DEFINED AVA_EXE)
  message(FATAL_ERROR "AVA_EXE is required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/test_timeout.cmake")
ava_test_timeout(AVA_PROCESS_TIMEOUT 10)

include("${CMAKE_CURRENT_LIST_DIR}/cli_test_common.cmake")

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef ROOT_SUFFIX)
set(TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/cli-packages-${ROOT_SUFFIX}")
ava_cli_isolated_env(AVA_CLI_ENV "${TEST_ROOT}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${AVA_CLI_ENV} "${AVA_EXE}" packages list
  OUTPUT_VARIABLE PACKAGE_OUTPUT
  ERROR_VARIABLE PACKAGE_ERROR
  RESULT_VARIABLE PACKAGE_RESULT
  TIMEOUT "${AVA_PROCESS_TIMEOUT}"
)

if(NOT PACKAGE_RESULT EQUAL 0)
  message(FATAL_ERROR "ava packages list exited with ${PACKAGE_RESULT}\nstdout:\n${PACKAGE_OUTPUT}\nstderr:\n${PACKAGE_ERROR}")
endif()

ava_cli_expect_contains("${PACKAGE_OUTPUT}" "package manager is deferred" "provenance" "docs/extensions/plugin-system.md")

file(REMOVE_RECURSE "${TEST_ROOT}")

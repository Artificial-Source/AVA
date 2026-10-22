if(NOT DEFINED AVA_EXE)
  message(FATAL_ERROR "AVA_EXE is required")
endif()

if(NOT DEFINED AVA_CLI_TEST_ROOT)
  message(FATAL_ERROR "AVA_CLI_TEST_ROOT is required")
endif()

# Timeouts for this driver. Honors AVA_DEBUG_NO_TIMEOUT at runtime (this
# script runs via `cmake -P`, so $ENV{...} is live) so a hung driver is not
# killed -- and does not kill its `ava` subprocess via the driver's EXIT
# trap -- before a debugger can be attached. Each AVA_TIMEOUT_<n> /
# AVA_POLL_<n> defaults to its authored value; setting AVA_DEBUG_NO_TIMEOUT
# stretches all of them to one hour (or AVA_DEBUG_NO_TIMEOUT_SECONDS). Poll
# caps are scaled at 20 iterations/second to match the sleep 0.05 loops.
set(AVA_TIMEOUT_15 15)
if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT})
  set(AVA_DEBUG_SECONDS 3600)
  if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS} AND "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}" MATCHES "^[1-9][0-9]*$")
    set(AVA_DEBUG_SECONDS "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}")
  endif()
  set(AVA_TIMEOUT_15 "${AVA_DEBUG_SECONDS}")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cli_test_common.cmake")
ava_cli_timeouts(15)

get_filename_component(TEST_ROOT "${AVA_CLI_TEST_ROOT}" ABSOLUTE)
set(WORKSPACE "${TEST_ROOT}/workspace")
set(INPUT_FILE "${TEST_ROOT}/rpc-input.jsonl")
set(OFF_INPUT_FILE "${TEST_ROOT}/rpc-off-input.jsonl")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY
     "${WORKSPACE}"
     "${TEST_ROOT}/home" "${TEST_ROOT}/config" "${TEST_ROOT}/state" "${TEST_ROOT}/data"
     "${TEST_ROOT}/off-home" "${TEST_ROOT}/off-config" "${TEST_ROOT}/off-state" "${TEST_ROOT}/off-data"
     "${TEST_ROOT}/invalid-home" "${TEST_ROOT}/invalid-config" "${TEST_ROOT}/invalid-state" "${TEST_ROOT}/invalid-data"
     "${TEST_ROOT}/missing-home" "${TEST_ROOT}/missing-config" "${TEST_ROOT}/missing-state" "${TEST_ROOT}/missing-data")
ava_cli_isolated_env(AVA_CLI_ENV "${TEST_ROOT}")
file(WRITE "${INPUT_FILE}"
     "{\"id\":\"state\",\"type\":\"get_state\"}\n"
     "{\"id\":\"stats\",\"type\":\"get_session_stats\"}\n"
     "{\"id\":\"new\",\"type\":\"new_session\"}\n")
file(WRITE "${OFF_INPUT_FILE}"
     "{\"id\":\"state\",\"type\":\"get_state\"}\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${AVA_CLI_ENV}
          "${AVA_EXE}" --mode rpc --thinking high
  WORKING_DIRECTORY "${WORKSPACE}"
  INPUT_FILE "${INPUT_FILE}"
  OUTPUT_VARIABLE AVA_OUTPUT
  ERROR_VARIABLE AVA_ERROR
  RESULT_VARIABLE AVA_RESULT
  TIMEOUT ${AVA_TIMEOUT_15}
)

if(NOT AVA_RESULT EQUAL 0)
  message(FATAL_ERROR "ava --thinking high --mode rpc exited with ${AVA_RESULT}\nstdout:\n${AVA_OUTPUT}\nstderr:\n${AVA_ERROR}")
endif()

ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.reasoning_enabled "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.reasoning_level "\"high\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "stats" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "stats" result.counts.reasoning_change "1")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "new" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "new" result.reasoning_enabled "false")
ava_rpc_forbid("${AVA_OUTPUT}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${AVA_CLI_ENV}
          "HOME=${TEST_ROOT}/off-home"
          "XDG_CONFIG_HOME=${TEST_ROOT}/off-config"
          "XDG_STATE_HOME=${TEST_ROOT}/off-state"
          "XDG_DATA_HOME=${TEST_ROOT}/off-data"
          "NO_COLOR=1"
          "${AVA_EXE}" --mode rpc --thinking off
  WORKING_DIRECTORY "${WORKSPACE}"
  INPUT_FILE "${OFF_INPUT_FILE}"
  OUTPUT_VARIABLE AVA_OFF_OUTPUT
  ERROR_VARIABLE AVA_OFF_ERROR
  RESULT_VARIABLE AVA_OFF_RESULT
  TIMEOUT ${AVA_TIMEOUT_15}
)

if(NOT AVA_OFF_RESULT EQUAL 0)
  message(FATAL_ERROR "ava --thinking off --mode rpc exited with ${AVA_OFF_RESULT}\nstdout:\n${AVA_OFF_OUTPUT}\nstderr:\n${AVA_OFF_ERROR}")
endif()

ava_rpc_expect_in_line("${AVA_OFF_OUTPUT}" "state" success "true")
ava_rpc_expect_in_line("${AVA_OFF_OUTPUT}" "state" result.reasoning_enabled "false")
ava_rpc_forbid("${AVA_OFF_OUTPUT}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${AVA_CLI_ENV}
          "HOME=${TEST_ROOT}/invalid-home"
          "XDG_CONFIG_HOME=${TEST_ROOT}/invalid-config"
          "XDG_STATE_HOME=${TEST_ROOT}/invalid-state"
          "XDG_DATA_HOME=${TEST_ROOT}/invalid-data"
          "NO_COLOR=1"
          "${AVA_EXE}" --mode rpc --thinking minimal
  WORKING_DIRECTORY "${WORKSPACE}"
  OUTPUT_VARIABLE AVA_INVALID_OUTPUT
  ERROR_VARIABLE AVA_INVALID_ERROR
  RESULT_VARIABLE AVA_INVALID_RESULT
  TIMEOUT ${AVA_TIMEOUT_15}
)

if(AVA_INVALID_RESULT EQUAL 0)
  message(FATAL_ERROR "ava --thinking minimal unexpectedly succeeded\nstdout:\n${AVA_INVALID_OUTPUT}\nstderr:\n${AVA_INVALID_ERROR}")
endif()

ava_cli_expect_contains("${AVA_INVALID_ERROR}"
        "reasoning level is not supported"
        "option: --thinking"
        "supported_levels: off, low, medium, high, xhigh")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${AVA_CLI_ENV}
          "HOME=${TEST_ROOT}/missing-home"
          "XDG_CONFIG_HOME=${TEST_ROOT}/missing-config"
          "XDG_STATE_HOME=${TEST_ROOT}/missing-state"
          "XDG_DATA_HOME=${TEST_ROOT}/missing-data"
          "NO_COLOR=1"
          "${AVA_EXE}" --mode rpc --thinking
  WORKING_DIRECTORY "${WORKSPACE}"
  OUTPUT_VARIABLE AVA_MISSING_OUTPUT
  ERROR_VARIABLE AVA_MISSING_ERROR
  RESULT_VARIABLE AVA_MISSING_RESULT
  TIMEOUT ${AVA_TIMEOUT_15}
)

if(AVA_MISSING_RESULT EQUAL 0)
  message(FATAL_ERROR "ava --thinking without a level unexpectedly succeeded\nstdout:\n${AVA_MISSING_OUTPUT}\nstderr:\n${AVA_MISSING_ERROR}")
endif()

string(FIND "${AVA_MISSING_ERROR}" "--thinking requires a reasoning level" MISSING_ERROR_INDEX)
if(MISSING_ERROR_INDEX EQUAL -1)
  message(FATAL_ERROR "ava --thinking missing-level stderr was not clear\nstdout:\n${AVA_MISSING_OUTPUT}\nstderr:\n${AVA_MISSING_ERROR}")
endif()

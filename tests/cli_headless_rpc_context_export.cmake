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
set(CURRENT_DIR "${WORKSPACE}/nested")
set(INPUT_FILE "${TEST_ROOT}/rpc-input.jsonl")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${CURRENT_DIR}" "${TEST_ROOT}/home" "${TEST_ROOT}/config" "${TEST_ROOT}/state" "${TEST_ROOT}/data")
ava_cli_isolated_env(AVA_CLI_ENV "${TEST_ROOT}")
file(REAL_PATH "${WORKSPACE}" REAL_WORKSPACE)
file(REAL_PATH "${CURRENT_DIR}" REAL_CURRENT_DIR)
file(WRITE "${WORKSPACE}/AGENTS.md" "# Headless Context\n\ncontext export smoke marker\n")
file(WRITE "${INPUT_FILE}"
     "{\"id\":\"state\",\"type\":\"get_state\",\"protocol_version\":1}\n"
     "{\"id\":\"ctx\",\"type\":\"context\"}\n"
     "{\"id\":\"exp\",\"type\":\"export\"}\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${AVA_CLI_ENV}
          "${AVA_EXE}" --cwd nested --rpc
  WORKING_DIRECTORY "${WORKSPACE}"
  INPUT_FILE "${INPUT_FILE}"
  OUTPUT_VARIABLE AVA_OUTPUT
  ERROR_VARIABLE AVA_ERROR
  RESULT_VARIABLE AVA_RESULT
  TIMEOUT ${AVA_TIMEOUT_15}
)

if(NOT AVA_RESULT EQUAL 0)
  message(FATAL_ERROR "ava --rpc exited with ${AVA_RESULT}\nstdout:\n${AVA_OUTPUT}\nstderr:\n${AVA_ERROR}")
endif()

ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.workspace_dir "\"${REAL_WORKSPACE}\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.current_dir "\"${REAL_CURRENT_DIR}\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.context_source_count "1")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.context_sources.0.source_type "\"workspace\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.context_sources.0.path "\"${REAL_WORKSPACE}/AGENTS.md\"")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "state")
ava_cli_expect_contains("${RESPONSE}"
        "\"workspace_dir\":\"${REAL_WORKSPACE}\""
        "\"current_dir\":\"${REAL_CURRENT_DIR}\""
        "\"path\":\"${REAL_WORKSPACE}/AGENTS.md\""
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "ctx" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "ctx" result.handled "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "ctx" result.quit "false")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "ctx")
ava_cli_expect_contains("${RESPONSE}"
        "workspace  ${REAL_WORKSPACE}/AGENTS.md"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "exp" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "exp")
ava_cli_expect_contains("${RESPONSE}"
        "# AVA Session Export"
        "## Session Start"
        "Provider"
        "Model"
)
ava_rpc_forbid("${AVA_OUTPUT}")

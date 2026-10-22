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

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${WORKSPACE}" "${TEST_ROOT}/home" "${TEST_ROOT}/config" "${TEST_ROOT}/state" "${TEST_ROOT}/data")
ava_cli_isolated_env(AVA_CLI_ENV "${TEST_ROOT}")
file(WRITE "${INPUT_FILE}"
     "not json\n"
     "{\"id\":\"badver\",\"type\":\"get_state\",\"protocol_version\":999}\n"
     "{\"id\":\"badver-type\",\"type\":\"get_state\",\"protocol_version\":\"1\"}\n"
     "{\"id\":\"missing-type\"}\n"
     "{\"id\":\"unknown\",\"type\":\"does_not_exist\"}\n"
     "{\"id\":\"reply-missing\",\"type\":\"permission_reply\"}\n"
     "{\"id\":\"state\",\"type\":\"get_state\",\"protocol_version\":1}\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${AVA_CLI_ENV}
          "${AVA_EXE}" --rpc
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

ava_rpc_expect_in_line("${AVA_OUTPUT}" "" success "false")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "" error.category "\"invalid_argument\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "" error.code "\"invalid_request\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "" error.message "\"malformed RPC JSON object\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "badver" success "false")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "badver" error.category "\"invalid_argument\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "badver" error.code "\"invalid_request\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "badver" error.message "\"unsupported RPC protocol version\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "badver-type" success "false")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "badver-type" error.category "\"invalid_argument\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "badver-type" error.code "\"invalid_request\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "badver-type" error.message "\"RPC protocol_version must be an integer\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "missing-type" success "false")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "missing-type" error.category "\"invalid_argument\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "missing-type" error.code "\"invalid_request\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "missing-type" error.message "\"RPC request requires a non-empty string type\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "unknown" success "false")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "unknown" error.category "\"invalid_argument\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "unknown" error.code "\"invalid_request\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "unknown" error.message "\"unknown RPC command type\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "reply-missing" success "false")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "reply-missing" error.category "\"invalid_argument\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "reply-missing" error.code "\"invalid_request\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "reply-missing" error.message "\"permission_reply requires request_id\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.protocol_version "1")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.cancel_requested "false")
ava_rpc_forbid("${AVA_OUTPUT}" "" "badver" "badver-type" "missing-type" "unknown" "reply-missing")

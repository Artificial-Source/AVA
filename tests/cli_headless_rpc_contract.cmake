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
file(REAL_PATH "${WORKSPACE}" REAL_WORKSPACE)
file(WRITE "${INPUT_FILE}"
     "{\"id\":\"proto\",\"type\":\"get_protocol\",\"protocol_version\":1}\n"
     "{\"id\":\"state\",\"type\":\"get_state\"}\n"
     "{\"id\":\"models\",\"type\":\"list_models\"}\n"
     "{\"id\":\"setr\",\"type\":\"set_reasoning\",\"reasoning_level\":\"low\"}\n"
     "{\"id\":\"clearr\",\"type\":\"clear_reasoning\"}\n"
     "{\"id\":\"set_model\",\"type\":\"set_model\",\"provider\":\"openai\",\"model\":\"gpt-4.1-mini\"}\n"
     "{\"id\":\"stats0\",\"type\":\"get_session_stats\"}\n"
     "{\"id\":\"validate0\",\"type\":\"validate_session\"}\n"
     "{\"id\":\"messages0\",\"type\":\"get_messages\"}\n"
     "{\"id\":\"sessions0\",\"type\":\"list_sessions\"}\n"
     "{\"id\":\"new\",\"type\":\"new_session\"}\n"
     "{\"id\":\"stats1\",\"type\":\"get_session_stats\"}\n"
     "{\"id\":\"validate1\",\"type\":\"validate_session\"}\n"
     "{\"id\":\"sessions1\",\"type\":\"list_sessions\"}\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${AVA_CLI_ENV}
          "${AVA_EXE}" --mode rpc
  WORKING_DIRECTORY "${WORKSPACE}"
  INPUT_FILE "${INPUT_FILE}"
  OUTPUT_VARIABLE AVA_OUTPUT
  ERROR_VARIABLE AVA_ERROR
  RESULT_VARIABLE AVA_RESULT
  TIMEOUT ${AVA_TIMEOUT_15}
)

if(NOT AVA_RESULT EQUAL 0)
  message(FATAL_ERROR "ava --mode rpc exited with ${AVA_RESULT}\nstdout:\n${AVA_OUTPUT}\nstderr:\n${AVA_ERROR}")
endif()

ava_rpc_expect_in_line("${AVA_OUTPUT}" "proto" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "proto" result.protocol_version "1")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.workspace_dir "\"${REAL_WORKSPACE}\"")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "state")
ava_cli_expect_contains("${RESPONSE}"
        "\"workspace_dir\":\"${REAL_WORKSPACE}\""
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "models" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "models" result.current_provider "\"openai\"")
ava_rpc_response_line(MODELS "${AVA_OUTPUT}" models)
ava_cli_array_item(MODEL "${MODELS}" result.models model "kimi-k2-thinking")
ava_cli_expect_json("${MODEL}" provider "\"kimi\"")
ava_cli_expect_json("${MODEL}" api_family "\"openai_chat_completions\"")
ava_cli_array_item(MODEL "${MODELS}" result.models model "kimi-k2.6")
ava_cli_expect_json("${MODEL}" provider "\"moonshot\"")
ava_cli_expect_json("${MODEL}" api_family "\"openai_chat_completions\"")
ava_cli_array_item(MODEL "${MODELS}" result.models model "moonshotai/kimi-k2.6")
ava_cli_expect_json("${MODEL}" provider "\"openrouter\"")
ava_cli_expect_json("${MODEL}" api_family "\"openai_chat_completions\"")
ava_cli_array_item(MODEL "${MODELS}" result.models model "kimi-k2-thinking")
ava_cli_expect_json("${MODEL}" reasoning_format "\"reasoning_content\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "setr" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "setr" result.reasoning_enabled "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "setr" result.reasoning_level "\"low\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "clearr" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "clearr" result.reasoning_enabled "false")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "set_model" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "set_model" result.model "\"gpt-4.1-mini\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "stats0" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "stats0" result.entry_count "4")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "stats0" result.counts.reasoning_change "2")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "stats0" result.counts.model_change "1")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate0" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate0" result.ok "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate0" result.error_count "0")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate0" result.issues "[]")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate1" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate1" result.ok "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate1" result.error_count "0")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate1" result.issues "[]")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "messages0" success "true")
ava_rpc_response_line(COLLECTION "${AVA_OUTPUT}" "messages0")
ava_cli_expect_type("${COLLECTION}" result.messages ARRAY)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "messages0" result.messages "[]")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "messages0" result.message_count "0")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "sessions0" success "true")
ava_rpc_response_line(COLLECTION "${AVA_OUTPUT}" "sessions0")
ava_cli_expect_type("${COLLECTION}" result.sessions ARRAY)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "new" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "stats1" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "stats1" result.entry_count "1")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "sessions1" success "true")
ava_rpc_response_line(COLLECTION "${AVA_OUTPUT}" "sessions1")
ava_cli_expect_type("${COLLECTION}" result.sessions ARRAY)
ava_rpc_forbid("${AVA_OUTPUT}")

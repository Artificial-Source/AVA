if(NOT DEFINED AVA_EXE)
  message(FATAL_ERROR "AVA_EXE is required")
endif()

if(NOT DEFINED AVA_FAKE_PROVIDER_EXE)
  message(FATAL_ERROR "AVA_FAKE_PROVIDER_EXE is required")
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
set(AVA_TIMEOUT_30 30)
set(AVA_POLL_200 200)
if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT})
  set(AVA_DEBUG_SECONDS 3600)
  if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS} AND "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}" MATCHES "^[1-9][0-9]*$")
    set(AVA_DEBUG_SECONDS "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}")
  endif()
  set(AVA_TIMEOUT_30 "${AVA_DEBUG_SECONDS}")
  math(EXPR AVA_POLL_200 "${AVA_DEBUG_SECONDS} * 20")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cli_test_common.cmake")
ava_cli_timeouts(30)
ava_cli_provider_paths()

get_filename_component(TEST_ROOT "${AVA_CLI_TEST_ROOT}" ABSOLUTE)
set(WORKSPACE "${TEST_ROOT}/workspace")
set(HOME_DIR "${TEST_ROOT}/home")
set(CONFIG_DIR "${TEST_ROOT}/config")
set(STATE_DIR "${TEST_ROOT}/state")
set(DATA_DIR "${TEST_ROOT}/data")
set(TARGET_FILE "${TEST_ROOT}/created-by-tool.txt")
set(PORT_FILE "${TEST_ROOT}/provider.port")
set(REQUEST_LOG "${TEST_ROOT}/provider-requests.log")
set(PROVIDER_OUT "${TEST_ROOT}/provider.out")
set(PROVIDER_ERR "${TEST_ROOT}/provider.err")
set(RPC_IN "${TEST_ROOT}/rpc-input.fifo")
set(RPC_OUT "${TEST_ROOT}/rpc-output.jsonl")
set(RPC_ERR "${TEST_ROOT}/rpc-error.log")
set(DRIVER_FILE "${TEST_ROOT}/driver.sh")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${WORKSPACE}" "${HOME_DIR}" "${CONFIG_DIR}/ava" "${STATE_DIR}" "${DATA_DIR}")
ava_cli_isolated_env(AVA_CLI_ENV "${TEST_ROOT}")
ava_cli_shell_env(AVA_CLI_ENV_SHELL ${AVA_CLI_ENV})
ava_cli_driver_support(AVA_CLI_DRIVER_SUPPORT "${TEST_ROOT}")
file(CHMOD "${CONFIG_DIR}/ava" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(WRITE "${CONFIG_DIR}/ava/models.json"
     "{\"default_provider\":\"moonshot\",\"default_model\":\"ava-headless-fake\","
     "\"models\":[{\"provider\":\"moonshot\",\"id\":\"ava-headless-fake\",\"family\":\"fake\",\"api_family\":\"openai_chat_completions\","
     "\"context_window_tokens\":8192,\"max_output_tokens\":1024,\"supports_tools\":true,"
     "\"supports_streaming\":false,\"supports_reasoning\":false,\"reports_usage\":true}]}\n")

file(WRITE "${DRIVER_FILE}"
"#!/bin/sh\n"
"set -u\n"
"AVA_PYTHON=\"${AVA_PYTHON}\"\n"
"AVA_FAKE_PROVIDER_PY=\"${AVA_FAKE_PROVIDER_PY}\"\n"
"AVA_FAKE_PROVIDER_SH=\"${AVA_FAKE_PROVIDER_SH}\"\n"
"AVA_FAKE_PROVIDER_EXE=\"${AVA_FAKE_PROVIDER_EXE}\"\n"
". \"${AVA_FAKE_PROVIDER_SH}\"\n"
". \"${AVA_CLI_DRIVER_SUPPORT}\"\n"
"rm -f \"${RPC_IN}\" \"${RPC_OUT}\" \"${RPC_ERR}\" \"${PORT_FILE}\" \"${REQUEST_LOG}\" \"${PROVIDER_OUT}\" \"${PROVIDER_ERR}\"\n"
"fake_provider_start \"${TEST_ROOT}\" provider 0 write-tool \"${TARGET_FILE}\" || exit 1\n"
"port=$FAKE_PROVIDER_PORT\n"
"mkfifo \"${RPC_IN}\"\n"
"setsid env ${AVA_CLI_ENV_SHELL} MOONSHOT_API_KEY=test-key MOONSHOT_BASE_URL=\"http://127.0.0.1:$port\" \"${AVA_EXE}\" --rpc < \"${RPC_IN}\" > \"${RPC_OUT}\" 2> \"${RPC_ERR}\" &\n"
"ava_pid=$!\n"
"exec 3>\"${RPC_IN}\"\n"
"printf '%s\\n' '{\"id\":\"prompt\",\"type\":\"prompt\",\"protocol_version\":1,\"message\":\"write outside with reply\"}' >&3\n"
"resolver_id=\n"
"i=0\n"
"while [ -z \"$resolver_id\" ]; do\n"
"  if [ -s \"${RPC_OUT}\" ]; then\n"
"    resolver_id=$(sed -n 's/.*\"resolver_request_id\":\"\\([^\"]*\\)\".*/\\1/p' \"${RPC_OUT}\" | head -n 1)\n"
"  fi\n"
"  if [ -n \"$resolver_id\" ]; then break; fi\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before permission request\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for permission request\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"printf '%s\\n' \"{\\\"id\\\":\\\"reply\\\",\\\"type\\\":\\\"permission_reply\\\",\\\"request_id\\\":\\\"$resolver_id\\\",\\\"correlation_id\\\":\\\"prompt\\\",\\\"decision\\\":\\\"deny\\\",\\\"reason\\\":\\\"not approved by headless client\\\"}\" >&3\n"
"i=0\n"
"while ! grep -q '\"id\":\"prompt\".*\"type\":\"response\".*\"success\":true' \"${RPC_OUT}\" 2>/dev/null; do\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before permission-deny prompt completed\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for permission-deny prompt completion\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"printf '%s\\n' '{\"id\":\"messages-after\",\"type\":\"get_messages\"}' >&3\n"
"ava_wait_response \"${RPC_OUT}\" \"prompt\" 30 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"messages-after\" 30 || exit 1\n"
"exec 3>&-\n"
"wait \"$ava_pid\"\n"
"ava_status=$?\n"
"ava_stop_child \"$ava_pid\"\n"
"ava_pid=\n"
"if [ \"$ava_status\" -ne 0 ]; then\n"
"  echo \"ava --rpc exited with $ava_status\" >&2\n"
"  cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"  cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"  exit \"$ava_status\"\n"
"fi\n"
"fake_provider_wait 1 ${AVA_TIMEOUT_30} || exit 1\n"
"fake_provider_finish ${AVA_TIMEOUT_30} || exit 1\n"
)

execute_process(
  COMMAND ${AVA_CLI_DRIVER_COMMAND} /bin/sh "${DRIVER_FILE}"
  WORKING_DIRECTORY "${WORKSPACE}"
  OUTPUT_VARIABLE DRIVER_OUTPUT
  ERROR_VARIABLE DRIVER_ERROR
  RESULT_VARIABLE DRIVER_RESULT
  TIMEOUT ${AVA_TIMEOUT_30}
)

if(NOT DRIVER_RESULT EQUAL 0)
  message(FATAL_ERROR "headless permission-reply driver exited with ${DRIVER_RESULT}\nstdout:\n${DRIVER_OUTPUT}\nstderr:\n${DRIVER_ERROR}")
endif()

file(READ "${RPC_OUT}" AVA_OUTPUT)
file(READ "${RPC_ERR}" AVA_ERROR)
file(READ "${REQUEST_LOG}" PROVIDER_REQUEST)

if(EXISTS "${TARGET_FILE}")
  message(FATAL_ERROR "denied write_file RPC permission flow unexpectedly created ${TARGET_FILE}\nstdout:\n${AVA_OUTPUT}\nstderr:\n${AVA_ERROR}")
endif()

ava_cli_expect_contains("${PROVIDER_REQUEST}"
        "--- request 1 ---"
        "--- request 2 ---"
        "\"model\":\"ava-headless-fake\""
        "\"stream\":false"
        "\"tool_call_id\":\"call_write\"")

ava_cli_event_line(EVENT "${AVA_OUTPUT}" "permission_requested")
ava_cli_expect_json("${EVENT}" payload.operation "\"edit\"")
ava_cli_expect_json("${EVENT}" payload.target_path "\"${TARGET_FILE}\"")
ava_cli_expect_json("${EVENT}" payload.tool_name "\"write_file\"")
ava_cli_expect_json("${EVENT}" payload.risk "\"high\"")
ava_cli_expect_contains("${EVENT}" "\"resolver_request_id\":\"permission_" "\"permission_request_id\":\"permreq_" "\"diff_preview\"" "+rpc new")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "reply" success "true")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "permission_replied")
ava_cli_expect_json("${EVENT}" payload.decision "\"deny\"")
ava_cli_expect_json("${EVENT}" payload.reason "\"not approved by headless client\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt" result.final_text "\"after permission deny\"")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "prompt")
ava_cli_expect_contains("${RESPONSE}"
        "resolution_reason: not approved by headless client"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "messages-after" success "true")
ava_rpc_response_line(COLLECTION "${AVA_OUTPUT}" "messages-after")
ava_cli_expect_type("${COLLECTION}" result.messages ARRAY)
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "messages-after")
ava_cli_expect_contains("${RESPONSE}"
        "\"type\":\"tool_result\""
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt" result.tool_calls "1")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt" result.tool_timeline.0.status "\"error\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt" result.tool_timeline.0.category "\"permission_denied\"")
ava_rpc_forbid("${AVA_OUTPUT}")

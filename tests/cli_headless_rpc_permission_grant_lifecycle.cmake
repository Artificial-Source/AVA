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
set(AVA_TIMEOUT_60 60)
set(AVA_POLL_200 200)
if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT})
  set(AVA_DEBUG_SECONDS 3600)
  if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS} AND "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}" MATCHES "^[1-9][0-9]*$")
    set(AVA_DEBUG_SECONDS "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}")
  endif()
  set(AVA_TIMEOUT_60 "${AVA_DEBUG_SECONDS}")
  math(EXPR AVA_POLL_200 "${AVA_DEBUG_SECONDS} * 20")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cli_test_common.cmake")
ava_cli_timeouts(60)
ava_cli_provider_paths()

get_filename_component(TEST_ROOT "${AVA_CLI_TEST_ROOT}" ABSOLUTE)
set(WORKSPACE "${TEST_ROOT}/workspace")
set(HOME_DIR "${TEST_ROOT}/home")
set(CONFIG_DIR "${TEST_ROOT}/config")
set(STATE_DIR "${TEST_ROOT}/state")
set(DATA_DIR "${TEST_ROOT}/data")
set(TARGET_FILE "${TEST_ROOT}/outside.txt")
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
file(WRITE "${TARGET_FILE}" "outside controlled grant target\n")
file(WRITE "${CONFIG_DIR}/ava/models.json"
     "{\"default_provider\":\"moonshot\",\"default_model\":\"ava-headless-fake\","
     "\"models\":[{\"provider\":\"moonshot\",\"id\":\"ava-headless-fake\",\"family\":\"fake\","
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
"fake_provider_start \"${TEST_ROOT}\" provider 0 read-tool-thrice \"${TARGET_FILE}\" || exit 1\n"
"port=$FAKE_PROVIDER_PORT\n"
"mkfifo \"${RPC_IN}\"\n"
"setsid env ${AVA_CLI_ENV_SHELL} MOONSHOT_API_KEY=test-key MOONSHOT_BASE_URL=\"http://127.0.0.1:$port\" \"${AVA_EXE}\" --rpc < \"${RPC_IN}\" > \"${RPC_OUT}\" 2> \"${RPC_ERR}\" &\n"
"ava_pid=$!\n"
"exec 3>\"${RPC_IN}\"\n"
"printf '%s\\n' '{\"id\":\"prompt1\",\"type\":\"prompt\",\"protocol_version\":1,\"message\":\"read outside first\"}' >&3\n"
"resolver_id=\n"
"i=0\n"
"while [ -z \"$resolver_id\" ]; do\n"
"  if [ -s \"${RPC_OUT}\" ]; then\n"
"    resolver_id=$(sed -n 's/.*\"resolver_request_id\":\"\\([^\"]*\\)\".*/\\1/p' \"${RPC_OUT}\" | head -n 1)\n"
"  fi\n"
"  if [ -n \"$resolver_id\" ]; then break; fi\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before first permission request\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for first permission request\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"printf '%s\\n' \"{\\\"id\\\":\\\"reply1\\\",\\\"type\\\":\\\"permission_reply\\\",\\\"request_id\\\":\\\"$resolver_id\\\",\\\"correlation_id\\\":\\\"prompt1\\\",\\\"decision\\\":\\\"allow_session\\\"}\" >&3\n"
"i=0\n"
"while ! grep -q '\"id\":\"prompt1\",\"type\":\"response\",\"success\":true' \"${RPC_OUT}\" 2>/dev/null; do\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before first prompt response\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for first prompt response\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"printf '%s\\n' '{\"id\":\"grants1\",\"type\":\"permission_grants\"}' >&3\n"
"grant_id=\n"
"i=0\n"
"while [ -z \"$grant_id\" ]; do\n"
"  if [ -s \"${RPC_OUT}\" ]; then\n"
"    grant_id=$(sed -n 's/.*\"grant_id\":\"\\([^\"]*\\)\".*/\\1/p' \"${RPC_OUT}\" | head -n 1)\n"
"  fi\n"
"  if [ -n \"$grant_id\" ]; then break; fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for first grant listing\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"printf '%s\\n' \"{\\\"id\\\":\\\"revoke\\\",\\\"type\\\":\\\"permission_grant_revoke\\\",\\\"grant_id\\\":\\\"$grant_id\\\"}\" >&3\n"
"i=0\n"
"while ! grep -q '\"id\":\"revoke\".*\"revoked\":true' \"${RPC_OUT}\" 2>/dev/null; do\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for grant revoke response\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"printf '%s\\n' '{\"id\":\"prompt2\",\"type\":\"prompt\",\"message\":\"read outside second\"}' >&3\n"
"resolver_id=\n"
"i=0\n"
"while [ -z \"$resolver_id\" ]; do\n"
"  permission_count=$(grep -c '\"name\":\"permission_requested\"' \"${RPC_OUT}\" 2>/dev/null || true)\n"
"  if [ \"$permission_count\" -ge 2 ]; then\n"
"    resolver_id=$(sed -n 's/.*\"resolver_request_id\":\"\\([^\"]*\\)\".*/\\1/p' \"${RPC_OUT}\" | tail -n 1)\n"
"  fi\n"
"  if [ -n \"$resolver_id\" ]; then break; fi\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before second permission request\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for second permission request after revoke\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"printf '%s\\n' \"{\\\"id\\\":\\\"reply2\\\",\\\"type\\\":\\\"permission_reply\\\",\\\"request_id\\\":\\\"$resolver_id\\\",\\\"correlation_id\\\":\\\"prompt2\\\",\\\"decision\\\":\\\"allow_session\\\"}\" >&3\n"
"i=0\n"
"while ! grep -q '\"id\":\"prompt2\",\"type\":\"response\",\"success\":true' \"${RPC_OUT}\" 2>/dev/null; do\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before second prompt response\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for second prompt response\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"printf '%s\\n' '{\"id\":\"clear\",\"type\":\"permission_grants_clear\"}' >&3\n"
"i=0\n"
"while ! grep -q '\"id\":\"clear\".*\"cleared\":1' \"${RPC_OUT}\" 2>/dev/null; do\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for grant clear response\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"printf '%s\\n' '{\"id\":\"prompt3\",\"type\":\"prompt\",\"message\":\"read outside third\"}' >&3\n"
"resolver_id=\n"
"i=0\n"
"while [ -z \"$resolver_id\" ]; do\n"
"  permission_count=$(grep -c '\"name\":\"permission_requested\"' \"${RPC_OUT}\" 2>/dev/null || true)\n"
"  if [ \"$permission_count\" -ge 3 ]; then\n"
"    resolver_id=$(sed -n 's/.*\"resolver_request_id\":\"\\([^\"]*\\)\".*/\\1/p' \"${RPC_OUT}\" | tail -n 1)\n"
"  fi\n"
"  if [ -n \"$resolver_id\" ]; then break; fi\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before third permission request\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for third permission request after clear\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"printf '%s\\n' \"{\\\"id\\\":\\\"reply3\\\",\\\"type\\\":\\\"permission_reply\\\",\\\"request_id\\\":\\\"$resolver_id\\\",\\\"correlation_id\\\":\\\"prompt3\\\",\\\"decision\\\":\\\"allow\\\"}\" >&3\n"
"i=0\n"
"while ! grep -q '\"id\":\"prompt3\",\"type\":\"response\",\"success\":true' \"${RPC_OUT}\" 2>/dev/null; do\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before third prompt response\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for third prompt response\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"ava_wait_response \"${RPC_OUT}\" \"prompt1\" 60 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"grants1\" 60 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"prompt2\" 60 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"clear\" 60 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"prompt3\" 60 || exit 1\n"
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
"fake_provider_wait 5 ${AVA_TIMEOUT_60} || exit 1\n"
"fake_provider_finish ${AVA_TIMEOUT_60} || exit 1\n"
)

execute_process(
  COMMAND ${AVA_CLI_DRIVER_COMMAND} /bin/sh "${DRIVER_FILE}"
  WORKING_DIRECTORY "${WORKSPACE}"
  OUTPUT_VARIABLE DRIVER_OUTPUT
  ERROR_VARIABLE DRIVER_ERROR
  RESULT_VARIABLE DRIVER_RESULT
  TIMEOUT ${AVA_TIMEOUT_60}
)

if(NOT DRIVER_RESULT EQUAL 0)
  message(FATAL_ERROR "headless permission-grant lifecycle driver exited with ${DRIVER_RESULT}\nstdout:\n${DRIVER_OUTPUT}\nstderr:\n${DRIVER_ERROR}")
endif()

file(READ "${RPC_OUT}" AVA_OUTPUT)
file(READ "${RPC_ERR}" AVA_ERROR)

string(REGEX MATCHALL "\"name\":\"permission_requested\"" PERMISSION_REQUEST_EVENTS "${AVA_OUTPUT}")
list(LENGTH PERMISSION_REQUEST_EVENTS PERMISSION_REQUEST_EVENT_COUNT)
if(NOT PERMISSION_REQUEST_EVENT_COUNT EQUAL 3)
  message(FATAL_ERROR "expected three permission requests across revoke and clear, got ${PERMISSION_REQUEST_EVENT_COUNT}\nstdout:\n${AVA_OUTPUT}\nstderr:\n${AVA_ERROR}")
endif()

ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt1" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt1" result.final_text "\"first controlled grant\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "reply1" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt2" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt2" result.final_text "\"second controlled grant\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "reply2" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt3" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt3" result.final_text "\"third controlled grant\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "reply3" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "grants1" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "revoke" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "revoke" result.revoked "true")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "permission_grant_revoked")
ava_cli_expect_json("${EVENT}" payload.revoked "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "clear" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "clear" result.cleared "1")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "permission_grants_cleared")
ava_cli_expect_json("${EVENT}" payload.cleared "1")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt1" result.tool_calls "1")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt1" result.tool_timeline.0.status "\"success\"")
ava_rpc_response_line(READ_RESULT "${AVA_OUTPUT}" prompt1)
ava_cli_expect_contains("${READ_RESULT}" "outside controlled grant target")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt2" result.tool_calls "1")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt2" result.tool_timeline.0.status "\"success\"")
ava_rpc_response_line(READ_RESULT "${AVA_OUTPUT}" prompt2)
ava_cli_expect_contains("${READ_RESULT}" "outside controlled grant target")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt3" result.tool_calls "1")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt3" result.tool_timeline.0.status "\"success\"")
ava_rpc_response_line(READ_RESULT "${AVA_OUTPUT}" prompt3)
ava_cli_expect_contains("${READ_RESULT}" "outside controlled grant target")
ava_rpc_forbid("${AVA_OUTPUT}")

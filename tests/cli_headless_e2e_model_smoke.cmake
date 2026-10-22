if(NOT DEFINED AVA_EXE)
  message(FATAL_ERROR "AVA_EXE is required")
endif()

if(NOT DEFINED AVA_FAKE_PROVIDER_EXE)
  message(FATAL_ERROR "AVA_FAKE_PROVIDER_EXE is required")
endif()

# Timeouts for this driver. Honors AVA_DEBUG_NO_TIMEOUT at runtime (this
# script runs via `cmake -P`, so $ENV{...} is live) so a hung driver is not
# killed -- and does not kill its `ava` subprocess via the driver's EXIT
# trap -- before a debugger can be attached. Each AVA_TIMEOUT_<n> /
# AVA_POLL_<n> defaults to its authored value; setting AVA_DEBUG_NO_TIMEOUT
# stretches all of them to one hour (or AVA_DEBUG_NO_TIMEOUT_SECONDS). Poll
# caps are scaled at 20 iterations/second to match the sleep 0.05 loops.
set(AVA_TIMEOUT_15 15)
set(AVA_TIMEOUT_45 45)
set(AVA_POLL_200 200)
set(AVA_POLL_400 400)
if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT})
  set(AVA_DEBUG_SECONDS 3600)
  if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS} AND "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}" MATCHES "^[1-9][0-9]*$")
    set(AVA_DEBUG_SECONDS "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}")
  endif()
  set(AVA_TIMEOUT_15 "${AVA_DEBUG_SECONDS}")
  set(AVA_TIMEOUT_45 "${AVA_DEBUG_SECONDS}")
  math(EXPR AVA_POLL_200 "${AVA_DEBUG_SECONDS} * 20")
  math(EXPR AVA_POLL_400 "${AVA_DEBUG_SECONDS} * 20")
endif()

# Command discovery seals the full ancestor chain of HOME. A root inside the
# build tree becomes stale when parallel CTest cases create sibling artifacts.
# mktemp atomically creates a private root under /tmp's sticky namespace; do
# not accept a supplied build-tree root or reuse a predictable /tmp path.
include("${CMAKE_CURRENT_LIST_DIR}/cli_test_common.cmake")
ava_cli_timeouts(15 45)
ava_cli_provider_paths()

execute_process(COMMAND mktemp -d /tmp/ava-cli-e2e-smoke.XXXXXXXX
                OUTPUT_VARIABLE TEST_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_VARIABLE TEMP_ERROR RESULT_VARIABLE TEMP_RESULT)
if(NOT TEMP_RESULT EQUAL 0 OR NOT TEST_ROOT MATCHES "^/tmp/ava-cli-e2e-smoke\\.[A-Za-z0-9]+$"
   OR NOT IS_DIRECTORY "${TEST_ROOT}" OR IS_SYMLINK "${TEST_ROOT}")
  message(FATAL_ERROR "failed to create private e2e smoke fixture: ${TEMP_ERROR}")
endif()
set(WORKSPACE "${TEST_ROOT}/workspace")
set(HOME_DIR "${TEST_ROOT}/home")
set(CONFIG_DIR "${TEST_ROOT}/config")
set(STATE_DIR "${TEST_ROOT}/state")
set(DATA_DIR "${TEST_ROOT}/data")
set(PORT_FILE "${TEST_ROOT}/provider.port")
set(REQUEST_LOG "${TEST_ROOT}/provider-requests.log")
set(PROVIDER_OUT "${TEST_ROOT}/provider.out")
set(PROVIDER_ERR "${TEST_ROOT}/provider.err")
set(RPC_IN "${TEST_ROOT}/rpc-input.fifo")
set(RPC_OUT "${TEST_ROOT}/rpc-output.jsonl")
set(RPC_ERR "${TEST_ROOT}/rpc-error.log")
set(REPLIED_IDS "${TEST_ROOT}/replied-permissions.txt")
set(REPLAY_INPUT "${TEST_ROOT}/replay-input.jsonl")
set(DRIVER_FILE "${TEST_ROOT}/driver.sh")
set(TARGET_FILE "${WORKSPACE}/src/todo.txt")
set(MUTATION_FILE "${TEST_ROOT}/outside-todo.txt")

file(MAKE_DIRECTORY "${WORKSPACE}/src" "${WORKSPACE}/docs" "${HOME_DIR}" "${CONFIG_DIR}/ava" "${STATE_DIR}/ava/sessions" "${DATA_DIR}")
ava_cli_isolated_env(AVA_CLI_ENV "${TEST_ROOT}")
ava_cli_shell_env(AVA_CLI_ENV_SHELL ${AVA_CLI_ENV})
ava_cli_driver_support(AVA_CLI_DRIVER_SUPPORT "${TEST_ROOT}")
# Model command sealing treats AVA's config and current-session namespace as
# authority roots, so this fixture must model the owner-private XDG layout.
file(CHMOD "${TEST_ROOT}" "${WORKSPACE}" "${HOME_DIR}" "${CONFIG_DIR}" "${CONFIG_DIR}/ava" "${STATE_DIR}" "${STATE_DIR}/ava" "${STATE_DIR}/ava/sessions" "${DATA_DIR}"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(WRITE "${WORKSPACE}/AGENTS.md" "headless e2e smoke project instructions\n")
file(WRITE "${TARGET_FILE}" "status: TODO\ndetail: replace TODO with DONE and verify.\n")
file(WRITE "${MUTATION_FILE}" "status: TODO\ndetail: replace TODO with DONE and verify.\n")
file(WRITE "${WORKSPACE}/docs/notes.md" "This file is outside the src include glob.\n")

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
"rm -f \"${RPC_IN}\" \"${RPC_OUT}\" \"${RPC_ERR}\" \"${PORT_FILE}\" \"${REQUEST_LOG}\" \"${PROVIDER_OUT}\" \"${PROVIDER_ERR}\" \"${REPLIED_IDS}\"\n"
"touch \"${REPLIED_IDS}\"\n"
"fake_provider_start \"${TEST_ROOT}\" provider 0 end-to-end-workflow ../outside-todo.txt || exit 1\n"
"port=$FAKE_PROVIDER_PORT\n"
"mkfifo \"${RPC_IN}\"\n"
"setsid env ${AVA_CLI_ENV_SHELL} MOONSHOT_API_KEY=test-key MOONSHOT_BASE_URL=\"http://127.0.0.1:$port\" \"${AVA_EXE}\" --rpc --allow read-only < \"${RPC_IN}\" > \"${RPC_OUT}\" 2> \"${RPC_ERR}\" &\n"
"ava_pid=$!\n"
"exec 3>\"${RPC_IN}\"\n"
"reply_permissions() {\n"
"  if [ ! -s \"${RPC_OUT}\" ]; then return 0; fi\n"
"  sed -n 's/.*\"resolver_request_id\":\"\\([^\"]*\\)\".*/\\1/p' \"${RPC_OUT}\" | while IFS= read -r resolver_id; do\n"
"    [ -n \"$resolver_id\" ] || continue\n"
"    if grep -qx \"$resolver_id\" \"${REPLIED_IDS}\" 2>/dev/null; then continue; fi\n"
"    printf '%s\\n' \"{\\\"id\\\":\\\"reply-$resolver_id\\\",\\\"type\\\":\\\"permission_reply\\\",\\\"request_id\\\":\\\"$resolver_id\\\",\\\"correlation_id\\\":\\\"prompt\\\",\\\"decision\\\":\\\"allow\\\",\\\"reason\\\":\\\"approved by deterministic e2e smoke\\\"}\" >&3\n"
"    echo \"$resolver_id\" >> \"${REPLIED_IDS}\"\n"
"  done\n"
"}\n"
"printf '%s\\n' '{\"id\":\"prompt\",\"type\":\"prompt\",\"protocol_version\":1,\"message\":\"Fix the TODO and verify the build.\"}' >&3\n"
"i=0\n"
"while ! grep -q '\"id\":\"prompt\".*\"type\":\"response\".*\"success\":true' \"${RPC_OUT}\" 2>/dev/null; do\n"
"  reply_permissions\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before e2e prompt completed\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_400} ]; then\n"
"    echo \"timed out waiting for e2e prompt completion\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"reply_permissions\n"
"printf '%s\\n' '{\"id\":\"stats-after\",\"type\":\"get_session_stats\"}' >&3\n"
"printf '%s\\n' '{\"id\":\"validate-after\",\"type\":\"validate_session\"}' >&3\n"
"printf '%s\\n' '{\"id\":\"messages-after\",\"type\":\"get_messages\"}' >&3\n"
"printf '%s\\n' '{\"id\":\"sessions-after\",\"type\":\"list_sessions\"}' >&3\n"
"ava_wait_response \"${RPC_OUT}\" \"prompt\" 45 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"stats-after\" 45 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"validate-after\" 45 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"messages-after\" 45 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"sessions-after\" 45 || exit 1\n"
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
"fake_provider_wait 5 ${AVA_TIMEOUT_45} || exit 1\n"
"fake_provider_finish ${AVA_TIMEOUT_45} || exit 1\n"
)

execute_process(
  COMMAND ${AVA_CLI_DRIVER_COMMAND} /bin/sh "${DRIVER_FILE}"
  WORKING_DIRECTORY "${WORKSPACE}"
  OUTPUT_VARIABLE DRIVER_OUTPUT
  ERROR_VARIABLE DRIVER_ERROR
  RESULT_VARIABLE DRIVER_RESULT
  TIMEOUT ${AVA_TIMEOUT_45}
)

if(NOT DRIVER_RESULT EQUAL 0)
  message(FATAL_ERROR "headless e2e model smoke driver exited with ${DRIVER_RESULT}\nstdout:\n${DRIVER_OUTPUT}\nstderr:\n${DRIVER_ERROR}")
endif()

file(READ "${RPC_OUT}" AVA_OUTPUT)
file(READ "${RPC_ERR}" AVA_ERROR)
file(READ "${REQUEST_LOG}" PROVIDER_REQUEST)
file(READ "${TARGET_FILE}" TARGET_CONTENT)
file(READ "${MUTATION_FILE}" MUTATION_CONTENT)

ava_cli_expect_contains("${TARGET_CONTENT}" "status: TODO")
ava_cli_expect_contains("${MUTATION_CONTENT}" "status: DONE")
ava_cli_expect_contains("${MUTATION_CONTENT}" "detail: replace TODO with DONE and verify.")

ava_cli_expect_contains("${PROVIDER_REQUEST}"
        "--- request 1 ---"
        "--- request 2 ---"
        "--- request 3 ---"
        "--- request 4 ---"
        "--- request 5 ---"
        "--- request 6 ---"
        "\"model\":\"ava-headless-fake\""
        "\"stream\":false"
        "\"name\":\"read_file\""
        "\"name\":\"grep\""
        "\"name\":\"list_directory\""
        "\"name\":\"apply_patch\""
        "\"name\":\"bash\"")

ava_cli_provider_request(REQUEST "${PROVIDER_REQUEST}" 2)
ava_cli_expect_contains("${REQUEST}" "\"tool_call_id\":\"call_read_e2e\"")
ava_cli_provider_request(REQUEST "${PROVIDER_REQUEST}" 2)
ava_cli_expect_contains("${REQUEST}" "\\\"tool\\\":\\\"read_file\\\"")
ava_cli_provider_request(REQUEST "${PROVIDER_REQUEST}" 3)
ava_cli_expect_contains("${REQUEST}" "\"tool_call_id\":\"call_grep_e2e\"")
ava_cli_provider_request(REQUEST "${PROVIDER_REQUEST}" 3)
ava_cli_expect_contains("${REQUEST}" "\\\"tool\\\":\\\"grep\\\"")
ava_cli_provider_request(REQUEST "${PROVIDER_REQUEST}" 4)
ava_cli_expect_contains("${REQUEST}" "\"tool_call_id\":\"call_list_e2e\"")
ava_cli_provider_request(REQUEST "${PROVIDER_REQUEST}" 4)
ava_cli_expect_contains("${REQUEST}" "\\\"tool\\\":\\\"list_directory\\\"")
ava_cli_provider_request(REQUEST "${PROVIDER_REQUEST}" 5)
ava_cli_expect_contains("${REQUEST}" "\"tool_call_id\":\"call_patch_e2e\"")
ava_cli_provider_request(REQUEST "${PROVIDER_REQUEST}" 5)
ava_cli_expect_contains("${REQUEST}" "\\\"tool\\\":\\\"apply_patch\\\"")
ava_cli_provider_request(REQUEST "${PROVIDER_REQUEST}" 6)
ava_cli_expect_contains("${REQUEST}" "\"tool_call_id\":\"call_bash_e2e\"")
ava_cli_provider_request(REQUEST "${PROVIDER_REQUEST}" 6)
ava_cli_expect_contains("${REQUEST}" "\\\"tool\\\":\\\"bash\\\"")
ava_cli_provider_request(REQUEST "${PROVIDER_REQUEST}" 6)
ava_cli_expect_contains("${REQUEST}" "status: DONE")

ava_cli_event_line(EVENT "${AVA_OUTPUT}" "tool_start" "call_read_e2e")
ava_cli_expect_json("${EVENT}" payload.tool "\"read_file\"")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "tool_result" "call_read_e2e")
ava_cli_expect_json("${EVENT}" payload.tool "\"read_file\"")
ava_cli_expect_json("${EVENT}" payload.status "\"success\"")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "tool_start" "call_grep_e2e")
ava_cli_expect_json("${EVENT}" payload.tool "\"grep\"")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "tool_result" "call_grep_e2e")
ava_cli_expect_json("${EVENT}" payload.tool "\"grep\"")
ava_cli_expect_json("${EVENT}" payload.status "\"success\"")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "tool_start" "call_list_e2e")
ava_cli_expect_json("${EVENT}" payload.tool "\"list_directory\"")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "tool_result" "call_list_e2e")
ava_cli_expect_json("${EVENT}" payload.tool "\"list_directory\"")
ava_cli_expect_json("${EVENT}" payload.status "\"success\"")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "tool_start" "call_patch_e2e")
ava_cli_expect_json("${EVENT}" payload.tool "\"apply_patch\"")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "tool_result" "call_patch_e2e")
ava_cli_expect_json("${EVENT}" payload.tool "\"apply_patch\"")
ava_cli_expect_json("${EVENT}" payload.status "\"success\"")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "tool_start" "call_bash_e2e")
ava_cli_expect_json("${EVENT}" payload.tool "\"bash\"")
ava_cli_event_line(EVENT "${AVA_OUTPUT}" "tool_result" "call_bash_e2e")
ava_cli_expect_json("${EVENT}" payload.tool "\"bash\"")
ava_cli_expect_json("${EVENT}" payload.status "\"success\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt" result.final_text "\"E2E task complete: TODO fixed and verification command passed.\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "stats-after" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "stats-after" result.counts.tool_call "5")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "stats-after" result.counts.tool_result "5")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate-after" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate-after" result.ok "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate-after" result.error_count "0")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "messages-after" success "true")
ava_rpc_response_line(COLLECTION "${AVA_OUTPUT}" "messages-after")
ava_cli_expect_type("${COLLECTION}" result.messages ARRAY)
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "messages-after")
ava_cli_expect_contains("${RESPONSE}"
        "\"type\":\"tool_result\""
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "sessions-after" success "true")
ava_rpc_response_line(COLLECTION "${AVA_OUTPUT}" "sessions-after")
ava_cli_expect_type("${COLLECTION}" result.sessions ARRAY)
ava_cli_event_line(PERMISSION "${AVA_OUTPUT}" permission_requested payload.operation edit)
ava_cli_expect_json("${PERMISSION}" payload.tool_name "\"apply_patch\"")
ava_cli_expect_contains("${PERMISSION}" "\"diff_preview\"" "+status: DONE")
string(JSON RESOLVER_ID GET "${PERMISSION}" payload resolver_request_id)
ava_cli_event_line(REPLY "${AVA_OUTPUT}" permission_replied payload.resolver_request_id "${RESOLVER_ID}")
ava_cli_expect_json("${REPLY}" payload.decision "\"allow\"")
ava_cli_expect_json("${REPLY}" payload.reason "\"approved by deterministic e2e smoke\"")
ava_cli_event_line(PERMISSION "${AVA_OUTPUT}" permission_requested payload.operation bash)
ava_cli_expect_json("${PERMISSION}" payload.tool_name "\"bash\"")
string(JSON RESOLVER_ID GET "${PERMISSION}" payload resolver_request_id)
ava_cli_event_line(REPLY "${AVA_OUTPUT}" permission_replied payload.resolver_request_id "${RESOLVER_ID}")
ava_cli_expect_json("${REPLY}" payload.decision "\"allow\"")
ava_cli_expect_json("${REPLY}" payload.reason "\"approved by deterministic e2e smoke\"")
ava_rpc_response_line(STATS "${AVA_OUTPUT}" "stats-after")
ava_cli_expect_type("${STATS}" result.counts.permission_decision NUMBER)
string(JSON DECISIONS GET "${STATS}" result counts permission_decision)
if(DECISIONS LESS 2)
  message(FATAL_ERROR "Expected audited patch and bash approvals: ${STATS}")
endif()
ava_rpc_forbid("${AVA_OUTPUT}")

ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "messages-after")
ava_cli_expect_contains("${RESPONSE}" "\"type\":\"assistant_message\"")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "messages-after")
ava_cli_expect_contains("${RESPONSE}" "E2E task complete: TODO fixed and verification command passed.")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "messages-after")
ava_cli_expect_contains("${RESPONSE}" "\"type\":\"tool_call\"")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "messages-after")
ava_cli_expect_contains("${RESPONSE}" "\"call_id\":\"call_bash_e2e\"")

file(GLOB_RECURSE SESSION_FILES "${STATE_DIR}/ava/sessions/*.jsonl")
list(LENGTH SESSION_FILES SESSION_FILE_COUNT)
if(NOT SESSION_FILE_COUNT EQUAL 1)
  message(FATAL_ERROR "expected one persisted session file, saw ${SESSION_FILE_COUNT}: ${SESSION_FILES}")
endif()
list(GET SESSION_FILES 0 SESSION_FILE)
file(READ "${SESSION_FILE}" SESSION_JSONL)
ava_cli_expect_contains("${SESSION_JSONL}"
        "\"type\":\"user_message\""
        "\"type\":\"assistant_output_item\""
        "\"type\":\"assistant_turn_commit\""
        "\"type\":\"tool_result\""
        "\"type\":\"permission_decision\""
        "\"tool_name\":\"apply_patch\""
        "\"tool_name\":\"bash\""
        "\"resolution\":\"allow\""
        "approved by deterministic e2e smoke")

string(REGEX MATCHALL "\"name\":\"permission_requested\"" PERMISSION_REQUEST_EVENTS "${AVA_OUTPUT}")
list(LENGTH PERMISSION_REQUEST_EVENTS PERMISSION_REQUEST_COUNT)
if(PERMISSION_REQUEST_COUNT LESS 2)
  message(FATAL_ERROR "expected at least apply_patch and bash permission requests, saw ${PERMISSION_REQUEST_COUNT}\nstdout:\n${AVA_OUTPUT}\nstderr:\n${AVA_ERROR}")
endif()

file(WRITE "${REPLAY_INPUT}"
     "{\"id\":\"replay-state\",\"type\":\"get_state\"}\n"
     "{\"id\":\"replay-stats\",\"type\":\"get_session_stats\"}\n"
     "{\"id\":\"replay-validate\",\"type\":\"validate_session\"}\n"
     "{\"id\":\"replay-messages\",\"type\":\"get_messages\"}\n"
     "{\"id\":\"replay-sessions\",\"type\":\"list_sessions\"}\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${AVA_CLI_ENV}
          "${AVA_EXE}" --rpc --continue
  WORKING_DIRECTORY "${WORKSPACE}"
  INPUT_FILE "${REPLAY_INPUT}"
  OUTPUT_VARIABLE REPLAY_OUTPUT
  ERROR_VARIABLE REPLAY_ERROR
  RESULT_VARIABLE REPLAY_RESULT
  TIMEOUT ${AVA_TIMEOUT_15}
)

if(NOT REPLAY_RESULT EQUAL 0)
  message(FATAL_ERROR "ava --rpc --continue exited with ${REPLAY_RESULT}\nstdout:\n${REPLAY_OUTPUT}\nstderr:\n${REPLAY_ERROR}")
endif()

ava_rpc_expect_in_line("${REPLAY_OUTPUT}" "replay-state" success "true")
ava_rpc_expect_in_line("${REPLAY_OUTPUT}" "replay-stats" success "true")
ava_rpc_expect_in_line("${REPLAY_OUTPUT}" "replay-stats" result.counts.tool_call "5")
ava_rpc_expect_in_line("${REPLAY_OUTPUT}" "replay-stats" result.counts.tool_result "5")
ava_rpc_expect_in_line("${REPLAY_OUTPUT}" "replay-validate" success "true")
ava_rpc_expect_in_line("${REPLAY_OUTPUT}" "replay-validate" result.ok "true")
ava_rpc_expect_in_line("${REPLAY_OUTPUT}" "replay-validate" result.error_count "0")
ava_rpc_expect_in_line("${REPLAY_OUTPUT}" "replay-messages" success "true")
ava_rpc_response_line(COLLECTION "${REPLAY_OUTPUT}" "replay-messages")
ava_cli_expect_type("${COLLECTION}" result.messages ARRAY)
ava_rpc_response_line(RESPONSE "${REPLAY_OUTPUT}" "replay-messages")
ava_cli_expect_contains("${RESPONSE}"
        "\"type\":\"tool_result\""
)
ava_rpc_expect_in_line("${REPLAY_OUTPUT}" "replay-sessions" success "true")
ava_rpc_response_line(COLLECTION "${REPLAY_OUTPUT}" "replay-sessions")
ava_cli_expect_type("${COLLECTION}" result.sessions ARRAY)
ava_rpc_expect_in_line("${REPLAY_OUTPUT}" "replay-messages" success "true")
ava_rpc_response_line(COLLECTION "${REPLAY_OUTPUT}" "replay-messages")
ava_cli_expect_type("${COLLECTION}" result.messages ARRAY)
ava_rpc_response_line(RESPONSE "${REPLAY_OUTPUT}" "replay-messages")
ava_cli_expect_contains("${RESPONSE}"
        "Fix the TODO and verify the build."
        "\"type\":\"user_message\""
        "status: DONE"
)
ava_rpc_response_line(STATS "${REPLAY_OUTPUT}" "replay-stats")
ava_cli_expect_type("${STATS}" result.counts.permission_decision NUMBER)
string(JSON DECISIONS GET "${STATS}" result counts permission_decision)
if(DECISIONS LESS 2)
  message(FATAL_ERROR "Expected audited patch and bash approvals: ${STATS}")
endif()
ava_rpc_forbid("${REPLAY_OUTPUT}")

ava_rpc_response_line(RESPONSE "${REPLAY_OUTPUT}" "replay-messages")
ava_cli_expect_contains("${RESPONSE}" "\"type\":\"assistant_message\"")
ava_rpc_response_line(RESPONSE "${REPLAY_OUTPUT}" "replay-messages")
ava_cli_expect_contains("${RESPONSE}" "E2E task complete: TODO fixed and verification command passed.")
ava_rpc_response_line(RESPONSE "${REPLAY_OUTPUT}" "replay-messages")
ava_cli_expect_contains("${RESPONSE}" "\"type\":\"tool_call\"")
ava_rpc_response_line(RESPONSE "${REPLAY_OUTPUT}" "replay-messages")
ava_cli_expect_contains("${RESPONSE}" "\"call_id\":\"call_bash_e2e\"")

file(REMOVE_RECURSE "${TEST_ROOT}")
if(EXISTS "${TEST_ROOT}" OR IS_SYMLINK "${TEST_ROOT}")
  message(FATAL_ERROR "failed to remove completed test fixture: ${TEST_ROOT}")
endif()

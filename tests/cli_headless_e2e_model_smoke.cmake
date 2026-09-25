if(NOT DEFINED AVA_EXE)
  message(FATAL_ERROR "AVA_EXE is required")
endif()

if(NOT DEFINED AVA_FAKE_PROVIDER_EXE)
  message(FATAL_ERROR "AVA_FAKE_PROVIDER_EXE is required")
endif()

if(NOT DEFINED AVA_CLI_TEST_ROOT)
  message(FATAL_ERROR "AVA_CLI_TEST_ROOT is required")
endif()

# The fake provider is launched through the shared Python owner/broker so every
# harness has identical process-gate wiring and process-group cleanup. Direct
# (non-CTest) invocations fall back to the script directory and PATH python3.
if(NOT DEFINED AVA_FAKE_PROVIDER_PY OR AVA_FAKE_PROVIDER_PY STREQUAL "")
  get_filename_component(AVA_FAKE_PROVIDER_PY "${CMAKE_CURRENT_LIST_DIR}/fake_provider.py" ABSOLUTE)
endif()
if(NOT DEFINED AVA_FAKE_PROVIDER_SH OR AVA_FAKE_PROVIDER_SH STREQUAL "")
  get_filename_component(AVA_FAKE_PROVIDER_SH "${CMAKE_CURRENT_LIST_DIR}/fake_provider_shell.sh" ABSOLUTE)
endif()
if(NOT DEFINED AVA_PYTHON OR AVA_PYTHON STREQUAL "")
  find_program(AVA_PYTHON NAMES python3 REQUIRED)
endif()

function(assert_contains VAR_NAME NEEDLE)
  string(FIND "${${VAR_NAME}}" "${NEEDLE}" NEEDLE_INDEX)
  if(NEEDLE_INDEX EQUAL -1)
    message(FATAL_ERROR "${VAR_NAME} did not contain ${NEEDLE}\ncontent:\n${${VAR_NAME}}")
  endif()
endfunction()

function(assert_after VAR_NAME ANCHOR NEEDLE)
  set(HAYSTACK "${${VAR_NAME}}")
  string(FIND "${HAYSTACK}" "${ANCHOR}" ANCHOR_INDEX)
  if(ANCHOR_INDEX EQUAL -1)
    message(FATAL_ERROR "${VAR_NAME} did not contain anchor ${ANCHOR}\ncontent:\n${HAYSTACK}")
  endif()
  string(LENGTH "${HAYSTACK}" HAYSTACK_LENGTH)
  math(EXPR TAIL_LENGTH "${HAYSTACK_LENGTH} - ${ANCHOR_INDEX}")
  string(SUBSTRING "${HAYSTACK}" ${ANCHOR_INDEX} ${TAIL_LENGTH} HAYSTACK_TAIL)
  string(FIND "${HAYSTACK_TAIL}" "${NEEDLE}" NEEDLE_INDEX)
  if(NEEDLE_INDEX EQUAL -1)
    message(FATAL_ERROR "${VAR_NAME} did not contain ${NEEDLE} after ${ANCHOR}\ncontent:\n${HAYSTACK}")
  endif()
endfunction()

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

# Use the CMake-supplied unique build-owned root. Do not copy into a predictable
# /tmp/<name> path: that races across runs, ignores isolated TMPDIR, and leaks
# after success. Failure artifacts stay in this bounded build directory; the
# next invocation starts with REMOVE_RECURSE on the same root.
get_filename_component(TEST_ROOT "${AVA_CLI_TEST_ROOT}" ABSOLUTE)
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

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${WORKSPACE}/src" "${WORKSPACE}/docs" "${HOME_DIR}" "${CONFIG_DIR}/ava" "${STATE_DIR}/ava/sessions" "${DATA_DIR}")
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
"ava_pid=\n"
"cleanup() {\n"
"  if [ -n \"$ava_pid\" ]; then kill \"$ava_pid\" 2>/dev/null || true; fi\n"
"  fake_provider_stop >/dev/null 2>&1 || true\n"
"}\n"
"trap cleanup EXIT INT TERM\n"
"rm -f \"${RPC_IN}\" \"${RPC_OUT}\" \"${RPC_ERR}\" \"${PORT_FILE}\" \"${REQUEST_LOG}\" \"${PROVIDER_OUT}\" \"${PROVIDER_ERR}\" \"${REPLIED_IDS}\"\n"
"touch \"${REPLIED_IDS}\"\n"
"fake_provider_start \"${TEST_ROOT}\" provider 0 end-to-end-workflow ../outside-todo.txt || exit 1\n"
"port=$FAKE_PROVIDER_PORT\n"
"mkfifo \"${RPC_IN}\"\n"
"HOME=\"${HOME_DIR}\" XDG_CONFIG_HOME=\"${CONFIG_DIR}\" XDG_STATE_HOME=\"${STATE_DIR}\" XDG_DATA_HOME=\"${DATA_DIR}\" NO_COLOR=1 MOONSHOT_API_KEY=test-key MOONSHOT_BASE_URL=\"http://127.0.0.1:$port\" \"${AVA_EXE}\" --rpc --allow read-only < \"${RPC_IN}\" > \"${RPC_OUT}\" 2> \"${RPC_ERR}\" &\n"
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
"while ! grep -q '\"final_text\":\"E2E task complete: TODO fixed and verification command passed.\"' \"${RPC_OUT}\" 2>/dev/null; do\n"
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
"exec 3>&-\n"
"wait \"$ava_pid\"\n"
"ava_status=$?\n"
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
  COMMAND /bin/sh "${DRIVER_FILE}"
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

assert_contains(TARGET_CONTENT "status: TODO")
assert_contains(MUTATION_CONTENT "status: DONE")
assert_contains(MUTATION_CONTENT "detail: replace TODO with DONE and verify.")

foreach(NEEDLE
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
  assert_contains(PROVIDER_REQUEST "${NEEDLE}")
endforeach()

assert_after(PROVIDER_REQUEST "--- request 2 ---" "\"tool_call_id\":\"call_read_e2e\"")
assert_after(PROVIDER_REQUEST "--- request 2 ---" "\\\"tool\\\":\\\"read_file\\\"")
assert_after(PROVIDER_REQUEST "--- request 3 ---" "\"tool_call_id\":\"call_grep_e2e\"")
assert_after(PROVIDER_REQUEST "--- request 3 ---" "\\\"tool\\\":\\\"grep\\\"")
assert_after(PROVIDER_REQUEST "--- request 4 ---" "\"tool_call_id\":\"call_list_e2e\"")
assert_after(PROVIDER_REQUEST "--- request 4 ---" "\\\"tool\\\":\\\"list_directory\\\"")
assert_after(PROVIDER_REQUEST "--- request 5 ---" "\"tool_call_id\":\"call_patch_e2e\"")
assert_after(PROVIDER_REQUEST "--- request 5 ---" "\\\"tool\\\":\\\"apply_patch\\\"")
assert_after(PROVIDER_REQUEST "--- request 6 ---" "\"tool_call_id\":\"call_bash_e2e\"")
assert_after(PROVIDER_REQUEST "--- request 6 ---" "\\\"tool\\\":\\\"bash\\\"")
assert_after(PROVIDER_REQUEST "--- request 6 ---" "status: DONE")

foreach(NEEDLE
        "\"name\":\"tool_start\""
        "\"name\":\"tool_result\""
        "\"call_id\":\"call_read_e2e\""
        "\"call_id\":\"call_grep_e2e\""
        "\"call_id\":\"call_list_e2e\""
        "\"call_id\":\"call_patch_e2e\""
        "\"call_id\":\"call_bash_e2e\""
        "\"tool\":\"read_file\""
        "\"tool\":\"grep\""
        "\"tool\":\"list_directory\""
        "\"tool\":\"apply_patch\""
        "\"tool\":\"bash\""
        "\"status\":\"success\""
        "status: TODO"
        "status: DONE"
        "\"diff_preview\""
        "+status: DONE"
        "\"name\":\"permission_requested\""
        "\"name\":\"permission_replied\""
        "\"decision\":\"allow\""
        "\"reason\":\"approved by deterministic e2e smoke\""
        "\"operation\":\"edit\""
        "\"operation\":\"bash\""
        "\"tool_name\":\"apply_patch\""
        "\"tool_name\":\"bash\""
        "\"id\":\"prompt\""
        "\"success\":true"
        "\"final_text\":\"E2E task complete: TODO fixed and verification command passed.\""
        "\"id\":\"stats-after\""
        "\"tool_call\":5"
        "\"tool_result\":5"
        "\"permission_decision\":"
        "\"id\":\"validate-after\""
        "\"ok\":true"
        "\"id\":\"messages-after\""
        "\"type\":\"user_message\""
        "\"name\":\"assistant_message\""
        "\"name\":\"tool_start\""
        "\"type\":\"tool_result\""
        "\"id\":\"sessions-after\""
        "\"sessions\":[")
  assert_contains(AVA_OUTPUT "${NEEDLE}")
endforeach()

assert_after(AVA_OUTPUT "\"id\":\"messages-after\"" "\"type\":\"assistant_message\"")
assert_after(AVA_OUTPUT "\"id\":\"messages-after\"" "E2E task complete: TODO fixed and verification command passed.")
assert_after(AVA_OUTPUT "\"id\":\"messages-after\"" "\"type\":\"tool_call\"")
assert_after(AVA_OUTPUT "\"id\":\"messages-after\"" "\"call_id\":\"call_bash_e2e\"")

file(GLOB_RECURSE SESSION_FILES "${STATE_DIR}/ava/sessions/*.jsonl")
list(LENGTH SESSION_FILES SESSION_FILE_COUNT)
if(NOT SESSION_FILE_COUNT EQUAL 1)
  message(FATAL_ERROR "expected one persisted session file, saw ${SESSION_FILE_COUNT}: ${SESSION_FILES}")
endif()
list(GET SESSION_FILES 0 SESSION_FILE)
file(READ "${SESSION_FILE}" SESSION_JSONL)
foreach(NEEDLE
        "\"type\":\"user_message\""
        "\"type\":\"assistant_output_item\""
        "\"type\":\"assistant_turn_commit\""
        "\"type\":\"tool_result\""
        "\"type\":\"permission_decision\""
        "\"tool_name\":\"apply_patch\""
        "\"tool_name\":\"bash\""
        "\"resolution\":\"allow\""
        "approved by deterministic e2e smoke")
  assert_contains(SESSION_JSONL "${NEEDLE}")
endforeach()

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
  COMMAND "${CMAKE_COMMAND}" -E env
          "HOME=${HOME_DIR}"
          "XDG_CONFIG_HOME=${CONFIG_DIR}"
          "XDG_STATE_HOME=${STATE_DIR}"
          "XDG_DATA_HOME=${DATA_DIR}"
          "NO_COLOR=1"
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

foreach(NEEDLE
        "\"id\":\"replay-state\""
        "\"id\":\"replay-stats\""
        "\"tool_call\":5"
        "\"tool_result\":5"
        "\"permission_decision\":"
        "\"id\":\"replay-validate\""
        "\"ok\":true"
        "\"id\":\"replay-messages\""
        "Fix the TODO and verify the build."
        "\"type\":\"user_message\""
        "\"type\":\"tool_result\""
        "status: DONE"
        "\"id\":\"replay-sessions\""
        "\"sessions\":[")
  assert_contains(REPLAY_OUTPUT "${NEEDLE}")
endforeach()

assert_after(REPLAY_OUTPUT "\"id\":\"replay-messages\"" "\"type\":\"assistant_message\"")
assert_after(REPLAY_OUTPUT "\"id\":\"replay-messages\"" "E2E task complete: TODO fixed and verification command passed.")
assert_after(REPLAY_OUTPUT "\"id\":\"replay-messages\"" "\"type\":\"tool_call\"")
assert_after(REPLAY_OUTPUT "\"id\":\"replay-messages\"" "\"call_id\":\"call_bash_e2e\"")

file(REMOVE_RECURSE "${TEST_ROOT}")
if(EXISTS "${TEST_ROOT}" OR IS_SYMLINK "${TEST_ROOT}")
  message(FATAL_ERROR "failed to remove completed test fixture: ${TEST_ROOT}")
endif()

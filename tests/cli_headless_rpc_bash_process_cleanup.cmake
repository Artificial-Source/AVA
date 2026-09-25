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

# Timeouts for this driver. Honors AVA_DEBUG_NO_TIMEOUT at runtime (this
# script runs via `cmake -P`, so $ENV{...} is live) so a hung driver is not
# killed -- and does not kill its `ava` subprocess via the driver's EXIT
# trap -- before a debugger can be attached. Each AVA_TIMEOUT_<n> /
# AVA_POLL_<n> defaults to its authored value; setting AVA_DEBUG_NO_TIMEOUT
# stretches all of them to one hour (or AVA_DEBUG_NO_TIMEOUT_SECONDS). Poll
# caps are scaled at 20 iterations/second to match the sleep 0.05 loops.
set(AVA_TIMEOUT_60 60)
set(AVA_POLL_100 100)
set(AVA_POLL_200 200)
if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT})
  set(AVA_DEBUG_SECONDS 3600)
  if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS} AND "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}" MATCHES "^[1-9][0-9]*$")
    set(AVA_DEBUG_SECONDS "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}")
  endif()
  set(AVA_TIMEOUT_60 "${AVA_DEBUG_SECONDS}")
  math(EXPR AVA_POLL_100 "${AVA_DEBUG_SECONDS} * 20")
  math(EXPR AVA_POLL_200 "${AVA_DEBUG_SECONDS} * 20")
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
set(MARKER_FILE "${TEST_ROOT}/bash-child-leak.txt")
set(PGID_FILE "${TEST_ROOT}/bash-child-pgid.txt")
set(PORT_FILE "${TEST_ROOT}/provider.port")
set(REQUEST_LOG "${TEST_ROOT}/provider-requests.log")
set(PROVIDER_OUT "${TEST_ROOT}/provider.out")
set(PROVIDER_ERR "${TEST_ROOT}/provider.err")
set(RPC_IN "${TEST_ROOT}/rpc-input.fifo")
set(RPC_OUT "${TEST_ROOT}/rpc-output.jsonl")
set(RPC_ERR "${TEST_ROOT}/rpc-error.log")
set(DRIVER_FILE "${TEST_ROOT}/driver.sh")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${WORKSPACE}" "${HOME_DIR}" "${CONFIG_DIR}/ava" "${STATE_DIR}/ava/sessions" "${DATA_DIR}")
# Command planning protects AVA config/session authority roots as well as the
# workspace; keep this process-cleanup fixture's XDG layout owner-private.
file(CHMOD "${TEST_ROOT}" "${WORKSPACE}" "${HOME_DIR}" "${CONFIG_DIR}" "${CONFIG_DIR}/ava" "${STATE_DIR}" "${STATE_DIR}/ava" "${STATE_DIR}/ava/sessions" "${DATA_DIR}"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
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
"ava_pid=\n"
"home_churn=\n"
"cleanup() {\n"
"  if [ -n \"$ava_pid\" ]; then kill \"$ava_pid\" 2>/dev/null || true; fi\n"
"  fake_provider_stop >/dev/null 2>&1 || true\n"
"  if [ -n \"$home_churn\" ]; then rmdir \"$home_churn\" 2>/dev/null || true; fi\n"
"  rm -f \"${PGID_FILE}\"\n"
"}\n"
"trap cleanup EXIT INT TERM\n"
"rm -f \"${RPC_IN}\" \"${RPC_OUT}\" \"${RPC_ERR}\" \"${PORT_FILE}\" \"${REQUEST_LOG}\" \"${PROVIDER_OUT}\" \"${PROVIDER_ERR}\" \"${MARKER_FILE}\" \"${PGID_FILE}\"\n"
"fake_provider_start \"${TEST_ROOT}\" provider 0 bash-timeout-tree \"${PGID_FILE}\" || exit 1\n"
"port=$FAKE_PROVIDER_PORT\n"
"mkfifo \"${RPC_IN}\"\n"
# Keep command-freshness inputs deterministic: this fixture validates process
# cleanup, not unrelated mutations in the invoking user's PATH directories.
"PATH=/usr/bin:/bin HOME=\"${HOME_DIR}\" XDG_CONFIG_HOME=\"${CONFIG_DIR}\" XDG_STATE_HOME=\"${STATE_DIR}\" XDG_DATA_HOME=\"${DATA_DIR}\" NO_COLOR=1 MOONSHOT_API_KEY=test-key MOONSHOT_BASE_URL=\"http://127.0.0.1:$port\" \"${AVA_EXE}\" --rpc < \"${RPC_IN}\" > \"${RPC_OUT}\" 2> \"${RPC_ERR}\" &\n"
"ava_pid=$!\n"
"exec 3>\"${RPC_IN}\"\n"
"printf '%s\\n' '{\"id\":\"prompt\",\"type\":\"prompt\",\"protocol_version\":1,\"message\":\"run timed shell tree\"}' >&3\n"
"resolver_id=\n"
"i=0\n"
"while [ -z \"$resolver_id\" ]; do\n"
"  if [ -s \"${RPC_OUT}\" ]; then\n"
"    resolver_id=$(sed -n 's/.*\"resolver_request_id\":\"\\([^\"]*\\)\".*/\\1/p' \"${RPC_OUT}\" | head -n 1)\n"
"  fi\n"
"  if [ -n \"$resolver_id\" ]; then break; fi\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before bash permission request\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for bash permission request\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"trusted_home=\"${HOME_DIR}\"\n"
"if [ -z \"$trusted_home\" ] || [ ! -d \"$trusted_home\" ]; then echo \"failed to resolve trusted account home fixture\" >&2; exit 1; fi\n"
"home_churn=\"$trusted_home/.ava-rpc-command-freshness-$$\"\n"
"if ! mkdir \"$home_churn\" || ! rmdir \"$home_churn\"; then echo \"failed to churn trusted-home entry fixture\" >&2; exit 1; fi\n"
"home_churn=\n"
"printf '%s\\n' \"{\\\"id\\\":\\\"reply\\\",\\\"type\\\":\\\"permission_reply\\\",\\\"request_id\\\":\\\"$resolver_id\\\",\\\"correlation_id\\\":\\\"prompt\\\",\\\"decision\\\":\\\"allow\\\"}\" >&3\n"
"i=0\n"
"while [ ! -s \"${PGID_FILE}\" ]; do\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before timed bash published its process group\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for bash process-group fixture\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"shell_pgid=$(cat \"${PGID_FILE}\")\n"
"case \"$shell_pgid\" in ''|*[!0-9]*) echo \"bash process-group fixture is not a valid PID\" >&2; exit 1;; esac\n"
"if [ \"$shell_pgid\" -le 1 ]; then echo \"bash process-group fixture is not a valid positive process group\" >&2; exit 1; fi\n"
"i=0\n"
"while ! grep -q 'after bash process cleanup' \"${RPC_OUT}\" 2>/dev/null; do\n"
"  if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"    echo \"ava exited before bash cleanup prompt completed\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"    echo \"timed out waiting for bash cleanup prompt completion\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"exec 3>&-\n"
"wait \"$ava_pid\"\n"
"ava_status=$?\n"
"ava_pid=\n"
"i=0\n"
"while kill -0 \"-$shell_pgid\" 2>/dev/null; do\n"
"  i=$((i + 1))\n"
"  if [ \"$i\" -gt ${AVA_POLL_100} ]; then\n"
"    echo \"timed-out bash process group $shell_pgid still exists after cleanup\" >&2\n"
"    cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"    cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"    exit 1\n"
"  fi\n"
"  sleep 0.05\n"
"done\n"
"if [ -e \"${MARKER_FILE}\" ]; then\n"
"  echo \"timed-out bash child process survived and wrote ${MARKER_FILE}\" >&2\n"
"  cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"  cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"  exit 1\n"
"fi\n"
"if [ \"$ava_status\" -ne 0 ]; then\n"
"  echo \"ava --rpc exited with $ava_status\" >&2\n"
"  cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"  cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"  exit \"$ava_status\"\n"
"fi\n"
"fake_provider_wait 1 ${AVA_TIMEOUT_60} || exit 1\n"
"fake_provider_finish ${AVA_TIMEOUT_60} || exit 1\n"
)

execute_process(
  COMMAND /bin/sh "${DRIVER_FILE}"
  WORKING_DIRECTORY "${WORKSPACE}"
  OUTPUT_VARIABLE DRIVER_OUTPUT
  ERROR_VARIABLE DRIVER_ERROR
  RESULT_VARIABLE DRIVER_RESULT
  TIMEOUT ${AVA_TIMEOUT_60}
)

if(NOT DRIVER_RESULT EQUAL 0)
  message(FATAL_ERROR "headless bash process-cleanup driver exited with ${DRIVER_RESULT}\nstdout:\n${DRIVER_OUTPUT}\nstderr:\n${DRIVER_ERROR}")
endif()

file(READ "${RPC_OUT}" AVA_OUTPUT)
file(READ "${RPC_ERR}" AVA_ERROR)

foreach(NEEDLE
        "\"name\":\"permission_requested\""
        "\"operation\":\"bash\""
        "\"name\":\"permission_replied\""
        "\"tool\":\"bash\""
        "\"timed_out\":true"
        "\"ok\":false"
        "after bash process cleanup")
  string(FIND "${AVA_OUTPUT}" "${NEEDLE}" NEEDLE_INDEX)
  if(NEEDLE_INDEX EQUAL -1)
    message(FATAL_ERROR "ava --rpc output did not contain ${NEEDLE}\nstdout:\n${AVA_OUTPUT}\nstderr:\n${AVA_ERROR}")
  endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
if(EXISTS "${TEST_ROOT}" OR IS_SYMLINK "${TEST_ROOT}")
  message(FATAL_ERROR "failed to remove completed test fixture: ${TEST_ROOT}")
endif()

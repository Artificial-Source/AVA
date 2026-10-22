if(NOT DEFINED AVA_EXE)
  message(FATAL_ERROR "AVA_EXE is required")
endif()

if(NOT DEFINED AVA_FAKE_PROVIDER_EXE)
  message(FATAL_ERROR "AVA_FAKE_PROVIDER_EXE is required")
endif()

if(NOT DEFINED AVA_FAKE_MCP_SERVER_EXE)
  message(FATAL_ERROR "AVA_FAKE_MCP_SERVER_EXE is required")
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
get_filename_component(FAKE_MCP_SERVER "${AVA_FAKE_MCP_SERVER_EXE}" ABSOLUTE)
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
set(DRIVER_FILE "${TEST_ROOT}/driver.sh")

string(REPLACE "\\" "\\\\" FAKE_MCP_SERVER_JSON "${FAKE_MCP_SERVER}")
string(REPLACE "\"" "\\\"" FAKE_MCP_SERVER_JSON "${FAKE_MCP_SERVER_JSON}")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${WORKSPACE}/.ava/commands" "${WORKSPACE}/.ava/skills/release" "${HOME_DIR}"
                    "${CONFIG_DIR}/ava" "${STATE_DIR}/ava" "${DATA_DIR}")
ava_cli_isolated_env(AVA_CLI_ENV "${TEST_ROOT}")
ava_cli_shell_env(AVA_CLI_ENV_SHELL ${AVA_CLI_ENV})
ava_cli_driver_support(AVA_CLI_DRIVER_SUPPORT "${TEST_ROOT}")
file(CHMOD "${CONFIG_DIR}/ava" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(REAL_PATH "${WORKSPACE}" REAL_WORKSPACE)
file(WRITE "${STATE_DIR}/ava/project-trust.json"
     "{\"schema_version\":1,\"decisions\":[{\"path\":\"${REAL_WORKSPACE}\",\"trusted\":true}]}\n")
file(WRITE "${CONFIG_DIR}/ava/models.json"
     "{\"default_provider\":\"moonshot\",\"default_model\":\"ava-headless-fake\","
     "\"models\":[{\"provider\":\"moonshot\",\"id\":\"ava-headless-fake\",\"family\":\"fake\","
     "\"context_window_tokens\":8192,\"max_output_tokens\":1024,\"supports_tools\":true,"
     "\"supports_streaming\":false,\"supports_reasoning\":false,\"reports_usage\":true}]}\n")
file(WRITE "${WORKSPACE}/.ava/commands/summarize.md"
     "---\ndescription: Summarize a topic\nargument-hint: <topic>\n---\nSummarize $1 for AVA command testing.\n")
file(WRITE "${WORKSPACE}/.ava/skills/release/SKILL.md"
     "---\nname: release\ndescription: Prepare release work\n---\nRelease skill body for headless command registry.\n")
file(WRITE "${WORKSPACE}/.ava/mcp.json"
     "{\"servers\":[{\"id\":\"demo\",\"name\":\"Demo MCP\",\"command\":\"${FAKE_MCP_SERVER_JSON}\","
     "\"enabled\":true}]}\n")

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
"fake_provider_start \"${TEST_ROOT}\" provider 0 text-three unused || exit 1\n"
"port=$FAKE_PROVIDER_PORT\n"
"mkfifo \"${RPC_IN}\"\n"
"setsid env ${AVA_CLI_ENV_SHELL} MOONSHOT_API_KEY=test-key MOONSHOT_BASE_URL=\"http://127.0.0.1:$port\" \"${AVA_EXE}\" --rpc --allow-tool skill,mcp < \"${RPC_IN}\" > \"${RPC_OUT}\" 2> \"${RPC_ERR}\" &\n"
"ava_pid=$!\n"
"exec 3>\"${RPC_IN}\"\n"
"wait_for() {\n"
"  needle=\"$1\"\n"
"  label=\"$2\"\n"
"  i=0\n"
"  while ! grep -q \"$needle\" \"${RPC_OUT}\" 2>/dev/null; do\n"
"    if ! kill -0 \"$ava_pid\" 2>/dev/null; then\n"
"      echo \"ava exited before $label\" >&2\n"
"      cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"      cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"      exit 1\n"
"    fi\n"
"    i=$((i + 1))\n"
"    if [ \"$i\" -gt ${AVA_POLL_200} ]; then\n"
"      echo \"timed out waiting for \\\"$label\\\"\" >&2\n"
"      cat \"${RPC_OUT}\" >&2 2>/dev/null || true\n"
"      cat \"${RPC_ERR}\" >&2 2>/dev/null || true\n"
"      exit 1\n"
"    fi\n"
"    sleep 0.05\n"
"  done\n"
"}\n"
"printf '%s\\n' '{\"id\":\"list\",\"type\":\"list_commands\",\"protocol_version\":1}' >&3\n"
"wait_for 'mcp:demo:release-notes' 'command list'\n"
"printf '%s\\n' '{\"id\":\"invoke-prompt\",\"type\":\"invoke_command\",\"name\":\"summarize\",\"command_arguments\":\"AVA-v1\"}' >&3\n"
"wait_for '\"id\":\"invoke-prompt\"' 'prompt command invocation'\n"
"printf '%s\\n' '{\"id\":\"invoke-skill\",\"type\":\"invoke_command\",\"name\":\"skill:release\"}' >&3\n"
"wait_for '\"id\":\"invoke-skill\"' 'skill command invocation'\n"
"printf '%s\\n' '{\"id\":\"invoke-mcp\",\"type\":\"invoke_command\",\"name\":\"release-notes\",\"command_arguments\":\"AVA\"}' >&3\n"
"wait_for '\"id\":\"invoke-mcp\"' 'MCP prompt command invocation'\n"
"ava_wait_response \"${RPC_OUT}\" \"list\" 30 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"invoke-prompt\" 30 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"invoke-skill\" 30 || exit 1\n"
"ava_wait_response \"${RPC_OUT}\" \"invoke-mcp\" 30 || exit 1\n"
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
"fake_provider_wait 2 ${AVA_TIMEOUT_30} || exit 1\n"
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
  message(FATAL_ERROR "headless command-registry driver exited with ${DRIVER_RESULT}\nstdout:\n${DRIVER_OUTPUT}\nstderr:\n${DRIVER_ERROR}")
endif()

file(READ "${RPC_OUT}" AVA_OUTPUT)
file(READ "${RPC_ERR}" AVA_ERROR)
file(READ "${REQUEST_LOG}" PROVIDER_REQUEST)

ava_rpc_expect_in_line("${AVA_OUTPUT}" "list" success "true")
ava_rpc_response_line(COMMANDS "${AVA_OUTPUT}" list)
ava_cli_array_item(COMMAND "${COMMANDS}" result.commands command "/summarize")
ava_cli_expect_json("${COMMAND}" source "\"prompt_project\"")
ava_cli_array_item(COMMAND "${COMMANDS}" result.commands command "/skill:release")
ava_cli_expect_json("${COMMAND}" source "\"skill\"")
ava_cli_array_item(COMMAND "${COMMANDS}" result.commands command "/mcp:demo:release-notes")
ava_cli_expect_json("${COMMAND}" source "\"mcp_prompt\"")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "invoke-prompt" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "invoke-skill" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "invoke-mcp" success "true")
ava_rpc_forbid("${AVA_OUTPUT}")

ava_cli_expect_contains("${PROVIDER_REQUEST}"
        "--- request 1 ---"
        "--- request 2 ---"
        "--- request 3 ---"
        "Summarize AVA-v1 for AVA command testing."
        "Release skill body for headless command registry."
        "MCP prompt for AVA")

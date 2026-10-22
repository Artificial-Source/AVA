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
set(PLUGIN_DIR "${WORKSPACE}/.ava/plugins/com.example.rpc")
set(BAD_PLUGIN_DIR "${WORKSPACE}/.ava/plugins/com.example.bad")
set(PLUGIN_MANIFEST "${PLUGIN_DIR}/plugin.json")
set(PLUGIN_SCRIPT "${PLUGIN_DIR}/plugin.sh")
set(PLUGIN_RAN_FILE "${PLUGIN_DIR}/ran.txt")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${PLUGIN_DIR}/prompts" "${PLUGIN_DIR}/skills" "${BAD_PLUGIN_DIR}" "${TEST_ROOT}/home"
                    "${TEST_ROOT}/config" "${TEST_ROOT}/state/ava" "${TEST_ROOT}/data")
ava_cli_isolated_env(AVA_CLI_ENV "${TEST_ROOT}")
file(REAL_PATH "${WORKSPACE}" REAL_WORKSPACE)
file(WRITE "${TEST_ROOT}/state/ava/project-trust.json"
     "{\"schema_version\":1,\"decisions\":[{\"path\":\"${REAL_WORKSPACE}\",\"trusted\":true}]}\n")
file(WRITE "${PLUGIN_MANIFEST}"
     [=[
{
  "schema_version": 1,
  "id": "com.example.rpc",
  "name": "RPC Plugin",
  "version": "0.1.0",
  "api_version": "ava.plugin.v1",
  "entrypoint": {"command": "/bin/sh", "args": ["plugin.sh"]},
  "capabilities": ["commands"],
  "contributes": {
    "commands": [{"name": "status", "description": "Show status"}],
    "prompts": [{"name": "review", "description": "Review prompt", "path": "prompts/review.md"}],
    "skills": [{"name": "triage", "description": "Triage skill", "path": "skills/triage.md"}]
  }
}
]=])
file(WRITE "${PLUGIN_SCRIPT}"
     "printf '%s\n' plugin-entrypoint-ran > '${PLUGIN_RAN_FILE}'\n"
     "while read line; do :; done\n")
file(WRITE "${PLUGIN_DIR}/prompts/review.md" "Prompt body from plugin\n")
file(WRITE "${PLUGIN_DIR}/skills/triage.md" "Skill body from plugin\n")
file(WRITE "${BAD_PLUGIN_DIR}/plugin.json" "{not-json\n")
file(WRITE "${INPUT_FILE}"
     "{\"id\":\"plugins\",\"type\":\"list_plugins\",\"protocol_version\":1}\n"
     "{\"id\":\"failures\",\"type\":\"plugin_failures\"}\n"
     "{\"id\":\"inspect\",\"type\":\"inspect_plugin\",\"plugin_id\":\"com.example.rpc\"}\n"
     "{\"id\":\"validate\",\"type\":\"validate_plugin\",\"path\":\".ava/plugins/com.example.rpc/plugin.json\"}\n"
     "{\"id\":\"prompts\",\"type\":\"list_plugin_prompts\",\"plugin_id\":\"com.example.rpc\"}\n"
     "{\"id\":\"prompt\",\"type\":\"get_plugin_prompt\",\"plugin_id\":\"com.example.rpc\",\"name\":\"review\"}\n"
     "{\"id\":\"skills\",\"type\":\"list_plugin_skills\",\"plugin_id\":\"com.example.rpc\"}\n"
     "{\"id\":\"skill\",\"type\":\"get_plugin_skill\",\"plugin_id\":\"com.example.rpc\",\"name\":\"triage\"}\n"
     "{\"id\":\"enable\",\"type\":\"enable_plugin\",\"plugin_id\":\"com.example.rpc\"}\n"
     "{\"id\":\"run\",\"type\":\"run_plugin_command\",\"plugin_id\":\"com.example.rpc\",\"name\":\"status\","
     "\"arguments\":\"{}\"}\n"
     "{\"id\":\"disable\",\"type\":\"disable_plugin\",\"plugin_id\":\"com.example.rpc\"}\n"
     "{\"id\":\"state\",\"type\":\"get_state\"}\n")

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

ava_rpc_expect_in_line("${AVA_OUTPUT}" "plugins" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "plugins")
ava_cli_expect_contains("${RESPONSE}"
        "com.example.rpc"
        "RPC Plugin"
        "0.1.0"
        "Failures: 1"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "failures" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "failures")
ava_cli_expect_contains("${RESPONSE}"
        "Plugin integration request was rejected [invalid_request]."
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "inspect" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "inspect")
ava_cli_expect_contains("${RESPONSE}"
        "entrypoint: /bin/sh plugin.sh (not executed)"
        "no plugin process is started yet"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "validate")
ava_cli_expect_contains("${RESPONSE}"
        "Valid plugin manifest"
        "no entrypoint was executed"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompts" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "prompts")
ava_cli_expect_contains("${RESPONSE}"
        "review - Review prompt"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "prompt")
ava_cli_expect_contains("${RESPONSE}"
        "Prompt body from plugin"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "skills" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "skills")
ava_cli_expect_contains("${RESPONSE}"
        "triage - Triage skill"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "skill" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "skill")
ava_cli_expect_contains("${RESPONSE}"
        "Skill body from plugin"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "enable" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "enable")
ava_cli_expect_contains("${RESPONSE}"
        "Enabled project plugin com.example.rpc."
        "No plugin process was started."
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "run" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "run")
ava_cli_expect_contains("${RESPONSE}"
        "permission_denied: plugin command requires permission"
        "plugin subprocess execution requires explicit approval"
        "command: com.example.rpc"
        "resolution: deny"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "disable" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "disable")
ava_cli_expect_contains("${RESPONSE}"
        "Disabled project plugin com.example.rpc."
        "No plugin process was stopped."
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.workspace_dir "\"${REAL_WORKSPACE}\"")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "state")
ava_cli_expect_contains("${RESPONSE}"
        "\"workspace_dir\":\"${REAL_WORKSPACE}\"")
ava_rpc_forbid("${AVA_OUTPUT}")

foreach(FORBIDDEN
        "plugin manifest must be a valid JSON object"
        "{not-json")
  string(FIND "${AVA_OUTPUT}${AVA_ERROR}" "${FORBIDDEN}" FORBIDDEN_INDEX)
  if(NOT FORBIDDEN_INDEX EQUAL -1)
    message(FATAL_ERROR "ava --rpc exposed raw plugin failure content: ${FORBIDDEN}\nstdout:\n${AVA_OUTPUT}\nstderr:\n${AVA_ERROR}")
  endif()
endforeach()

if(EXISTS "${PLUGIN_RAN_FILE}")
  message(FATAL_ERROR "fail-closed run_plugin_command executed plugin entrypoint unexpectedly\nstdout:\n${AVA_OUTPUT}")
endif()

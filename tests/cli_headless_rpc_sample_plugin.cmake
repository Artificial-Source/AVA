if(NOT DEFINED AVA_EXE)
  message(FATAL_ERROR "AVA_EXE is required")
endif()

if(NOT DEFINED AVA_CLI_TEST_ROOT)
  message(FATAL_ERROR "AVA_CLI_TEST_ROOT is required")
endif()

if(NOT DEFINED AVA_SAMPLE_TODO_PLUGIN_DIR)
  message(FATAL_ERROR "AVA_SAMPLE_TODO_PLUGIN_DIR is required")
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
get_filename_component(SAMPLE_TODO_PLUGIN_DIR "${AVA_SAMPLE_TODO_PLUGIN_DIR}" ABSOLUTE)
set(WORKSPACE "${TEST_ROOT}/workspace")
set(INPUT_FILE "${TEST_ROOT}/rpc-input.jsonl")
set(PLUGIN_DIR "${WORKSPACE}/.ava/plugins/com.example.todo")
set(PLUGIN_SCRIPT "${PLUGIN_DIR}/plugin.sh")
set(PLUGIN_RAN_FILE "${PLUGIN_DIR}/entrypoint-ran")

if(NOT EXISTS "${SAMPLE_TODO_PLUGIN_DIR}/plugin.json")
  message(FATAL_ERROR "sample todo plugin manifest not found: ${SAMPLE_TODO_PLUGIN_DIR}/plugin.json")
endif()

if(NOT EXISTS "${SAMPLE_TODO_PLUGIN_DIR}/plugin.sh")
  message(FATAL_ERROR "sample todo plugin entrypoint not found: ${SAMPLE_TODO_PLUGIN_DIR}/plugin.sh")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${WORKSPACE}/.ava/plugins" "${TEST_ROOT}/home" "${TEST_ROOT}/config" "${TEST_ROOT}/state/ava"
                    "${TEST_ROOT}/data")
ava_cli_isolated_env(AVA_CLI_ENV "${TEST_ROOT}")
file(COPY "${SAMPLE_TODO_PLUGIN_DIR}/" DESTINATION "${PLUGIN_DIR}")
file(READ "${PLUGIN_SCRIPT}" PLUGIN_SCRIPT_ORIGINAL)
file(WRITE "${PLUGIN_SCRIPT}" "printf '%s\\n' plugin-entrypoint-ran > '${PLUGIN_RAN_FILE}'\n${PLUGIN_SCRIPT_ORIGINAL}")
file(REAL_PATH "${WORKSPACE}" REAL_WORKSPACE)
file(WRITE "${TEST_ROOT}/state/ava/project-trust.json"
     "{\"schema_version\":1,\"decisions\":[{\"path\":\"${REAL_WORKSPACE}\",\"trusted\":true}]}\n")

file(WRITE "${INPUT_FILE}"
     "{\"id\":\"plugins\",\"type\":\"list_plugins\",\"protocol_version\":1}\n"
     "{\"id\":\"inspect\",\"type\":\"inspect_plugin\",\"plugin_id\":\"com.example.todo\"}\n"
     "{\"id\":\"validate\",\"type\":\"validate_plugin\",\"path\":\".ava/plugins/com.example.todo/plugin.json\"}\n"
     "{\"id\":\"prompts\",\"type\":\"list_plugin_prompts\",\"plugin_id\":\"com.example.todo\"}\n"
     "{\"id\":\"prompt\",\"type\":\"get_plugin_prompt\",\"plugin_id\":\"com.example.todo\",\"name\":\"todo-review\"}\n"
     "{\"id\":\"skills\",\"type\":\"list_plugin_skills\",\"plugin_id\":\"com.example.todo\"}\n"
     "{\"id\":\"skill\",\"type\":\"get_plugin_skill\",\"plugin_id\":\"com.example.todo\",\"name\":\"todo-triage\"}\n"
     "{\"id\":\"enable\",\"type\":\"enable_plugin\",\"plugin_id\":\"com.example.todo\"}\n"
     "{\"id\":\"run\",\"type\":\"run_plugin_command\",\"plugin_id\":\"com.example.todo\",\"name\":\"status\","
     "\"arguments\":\"{}\"}\n"
     "{\"id\":\"disable\",\"type\":\"disable_plugin\",\"plugin_id\":\"com.example.todo\"}\n"
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
        "com.example.todo"
        "Todo Sample Plugin"
        "0.1.0"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "inspect" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "inspect")
ava_cli_expect_contains("${RESPONSE}"
        "entrypoint: /bin/sh plugin.sh (not executed)"
        "capabilities: tools, commands, prompts, skills, event_hooks"
        "todo_add - Accept one todo item for the current AVA call."
        "status - Report that the sample plugin is ready."
        "todo-review - Review todo-oriented follow-up work."
        "todo-triage - Triage a small implementation todo list."
        "event_hooks: 1"
        "no plugin process is started yet"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "validate" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "validate")
ava_cli_expect_contains("${RESPONSE}"
        "Valid plugin manifest"
        "api_version: ava.plugin.v1"
        "tools: 1"
        "commands: 1"
        "prompts: 1"
        "skills: 1"
        "note: validation parsed the manifest only; no entrypoint was executed."
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompts" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "prompts")
ava_cli_expect_contains("${RESPONSE}"
        "Plugin prompts for com.example.todo"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "prompt" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "prompt")
ava_cli_expect_contains("${RESPONSE}"
        "Plugin prompt com.example.todo/todo-review"
        "# Todo Review Prompt"
        "Identify the next concrete action."
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "skills" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "skills")
ava_cli_expect_contains("${RESPONSE}"
        "Plugin skills for com.example.todo"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "skill" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "skill")
ava_cli_expect_contains("${RESPONSE}"
        "Plugin skill com.example.todo/todo-triage"
        "# Todo Triage Skill"
        "Group related todos by outcome."
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "enable" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "enable")
ava_cli_expect_contains("${RESPONSE}"
        "Enabled project plugin com.example.todo. No plugin process was started."
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "run" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "run")
ava_cli_expect_contains("${RESPONSE}"
        "permission_denied: plugin command requires permission"
        "plugin subprocess execution requires explicit approval"
        "command: com.example.todo"
        "resolution: deny"
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "disable" success "true")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "disable")
ava_cli_expect_contains("${RESPONSE}"
        "Disabled project plugin com.example.todo. No plugin process was stopped."
)
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" success "true")
ava_rpc_expect_in_line("${AVA_OUTPUT}" "state" result.workspace_dir "\"${REAL_WORKSPACE}\"")
ava_rpc_response_line(RESPONSE "${AVA_OUTPUT}" "state")
ava_cli_expect_contains("${RESPONSE}"
        "\"workspace_dir\":\"${REAL_WORKSPACE}\""
        "\"current_dir\":\"${REAL_WORKSPACE}\"")
ava_rpc_forbid("${AVA_OUTPUT}")

if(EXISTS "${PLUGIN_RAN_FILE}")
  message(FATAL_ERROR "fail-closed run_plugin_command executed sample plugin entrypoint unexpectedly\nstdout:\n${AVA_OUTPUT}")
endif()

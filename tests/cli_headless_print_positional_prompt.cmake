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
set(PORT_FILE "${TEST_ROOT}/provider.port")
set(REQUEST_LOG "${TEST_ROOT}/provider-requests.log")
set(PROVIDER_OUT "${TEST_ROOT}/provider.out")
set(PROVIDER_ERR "${TEST_ROOT}/provider.err")
set(AVA_OUT "${TEST_ROOT}/ava.out")
set(AVA_ERR "${TEST_ROOT}/ava.err")
set(DRIVER_FILE "${TEST_ROOT}/driver.sh")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${WORKSPACE}" "${HOME_DIR}" "${CONFIG_DIR}/ava/agents" "${STATE_DIR}" "${DATA_DIR}")
ava_cli_isolated_env(AVA_CLI_ENV "${TEST_ROOT}")
ava_cli_shell_env(AVA_CLI_ENV_SHELL ${AVA_CLI_ENV})
ava_cli_driver_support(AVA_CLI_DRIVER_SUPPORT "${TEST_ROOT}")
file(WRITE "${WORKSPACE}/AGENTS.md" "workspace positional context should remain\n")
file(WRITE "${CONFIG_DIR}/ava/agents/coder.md"
     "---\nname: coder\ndescription: Selected read-only coder.\nmode: primary\ntools: read-only\n---\nSELECTED_PRIMARY_CODER_CANARY\n")
file(WRITE "${CONFIG_DIR}/ava/agents/task-only.md"
     "---\nname: task-only\ndescription: Task-only definition.\nmode: subagent\n---\nTASK_ONLY_CANARY\n")
file(WRITE "${CONFIG_DIR}/ava/agents/dashy.md"
     "---\nname: -dashy\ndescription: Leading-dash primary definition.\nmode: primary\n---\nDASHY_PRIMARY_CANARY\n")
file(WRITE "${CONFIG_DIR}/ava/agents/broken.md"
     "---\nname: broken\ndescription: Malformed primary definition.\nmode: primary\ntools: unrestricted\n---\nBROKEN_PRIMARY_CANARY\n")
file(WRITE "${CONFIG_DIR}/ava/models.json"
     "{\"default_provider\":\"moonshot\",\"default_model\":\"ava-headless-fake\","
     "\"models\":[{\"provider\":\"moonshot\",\"id\":\"ava-headless-fake\",\"family\":\"fake\","
     "\"context_window_tokens\":8192,\"max_output_tokens\":1024,\"supports_tools\":true,"
     "\"supports_streaming\":false,\"supports_reasoning\":false,\"reports_usage\":true}]}\n")

execute_process(COMMAND "${AVA_EXE}" --print --agent RESULT_VARIABLE MISSING_AGENT_STATUS ERROR_VARIABLE MISSING_AGENT_ERROR)
if(NOT MISSING_AGENT_STATUS EQUAL 2 OR NOT MISSING_AGENT_ERROR MATCHES "--agent requires one agent name")
  message(FATAL_ERROR "missing --agent value was not rejected deterministically: ${MISSING_AGENT_STATUS}: ${MISSING_AGENT_ERROR}")
endif()
execute_process(COMMAND "${AVA_EXE}" --agent coder --agent coder --print prompt RESULT_VARIABLE DUPLICATE_AGENT_STATUS ERROR_VARIABLE DUPLICATE_AGENT_ERROR)
if(NOT DUPLICATE_AGENT_STATUS EQUAL 2 OR NOT DUPLICATE_AGENT_ERROR MATCHES "--agent may be specified only once")
  message(FATAL_ERROR "duplicate --agent was not rejected deterministically: ${DUPLICATE_AGENT_STATUS}: ${DUPLICATE_AGENT_ERROR}")
endif()
execute_process(COMMAND "${AVA_EXE}" --acp --agent coder RESULT_VARIABLE ACP_AGENT_STATUS ERROR_VARIABLE ACP_AGENT_ERROR)
if(NOT ACP_AGENT_STATUS EQUAL 2 OR NOT ACP_AGENT_ERROR MATCHES "--acp is a standalone mode")
  message(FATAL_ERROR "ACP accepted --agent unexpectedly: ${ACP_AGENT_STATUS}: ${ACP_AGENT_ERROR}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${AVA_CLI_ENV}
          "HOME=${HOME_DIR}"
          "XDG_CONFIG_HOME=${CONFIG_DIR}"
          "XDG_STATE_HOME=${STATE_DIR}"
          "XDG_DATA_HOME=${DATA_DIR}"
          "MOONSHOT_API_KEY=test-key"
          "${AVA_EXE}" --offline --no-session --print --agent -dashy prompt
  WORKING_DIRECTORY "${WORKSPACE}"
  RESULT_VARIABLE DASHY_AGENT_STATUS
  ERROR_VARIABLE DASHY_AGENT_ERROR
)
if(NOT DASHY_AGENT_STATUS EQUAL 1 OR NOT DASHY_AGENT_ERROR MATCHES "offline mode is enabled")
  message(FATAL_ERROR "grammar-valid leading-dash agent was not selectable: ${DASHY_AGENT_STATUS}: ${DASHY_AGENT_ERROR}")
endif()

file(WRITE "${DRIVER_FILE}"
"#!/bin/sh\n"
"set -u\n"
"AVA_PYTHON=\"${AVA_PYTHON}\"\n"
"AVA_FAKE_PROVIDER_PY=\"${AVA_FAKE_PROVIDER_PY}\"\n"
"AVA_FAKE_PROVIDER_SH=\"${AVA_FAKE_PROVIDER_SH}\"\n"
"AVA_FAKE_PROVIDER_EXE=\"${AVA_FAKE_PROVIDER_EXE}\"\n"
". \"${AVA_FAKE_PROVIDER_SH}\"\n"
". \"${AVA_CLI_DRIVER_SUPPORT}\"\n"
"rm -f \"${PORT_FILE}\" \"${REQUEST_LOG}\" \"${PROVIDER_OUT}\" \"${PROVIDER_ERR}\" \"${AVA_OUT}\" \"${AVA_ERR}\"\n"
"fake_provider_start \"${TEST_ROOT}\" provider 0 text unused || exit 1\n"
"port=$FAKE_PROVIDER_PORT\n"
"for invalid_agent in unknown task-only broken; do\n"
"  ava_run env ${AVA_CLI_ENV_SHELL} MOONSHOT_API_KEY=test-key MOONSHOT_BASE_URL=\"http://127.0.0.1:$port\" \"${AVA_EXE}\" --print --agent \"$invalid_agent\" prompt > /dev/null 2> \"${AVA_ERR}\" && { echo \"invalid selected agent unexpectedly succeeded\" >&2; exit 1; }\n"
"  if [ -s \"${REQUEST_LOG}\" ]; then echo \"invalid selected agent reached provider\" >&2; exit 1; fi\n"
"done\n"
"ava_run env ${AVA_CLI_ENV_SHELL} MOONSHOT_API_KEY=test-key MOONSHOT_BASE_URL=\"http://127.0.0.1:$port\" \"${AVA_EXE}\" --print --agent coder \"hello\" \"positional\" \"prompt\" --output text > \"${AVA_OUT}\" 2> \"${AVA_ERR}\"\n"
"ava_status=$?\n"
"if [ \"$ava_status\" -ne 0 ]; then\n"
"  echo \"ava positional prompt exited with $ava_status\" >&2\n"
"  cat \"${AVA_OUT}\" >&2 2>/dev/null || true\n"
"  cat \"${AVA_ERR}\" >&2 2>/dev/null || true\n"
"  exit \"$ava_status\"\n"
"fi\n"
"fake_provider_wait 0 ${AVA_TIMEOUT_30} || exit 1\n"
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
  message(FATAL_ERROR "headless positional prompt driver exited with ${DRIVER_RESULT}\nstdout:\n${DRIVER_OUTPUT}\nstderr:\n${DRIVER_ERROR}")
endif()

file(READ "${AVA_OUT}" AVA_OUTPUT)
file(READ "${AVA_ERR}" AVA_ERROR)
file(READ "${REQUEST_LOG}" PROVIDER_REQUEST)
file(GLOB_RECURSE SESSION_FILES "${STATE_DIR}/ava/sessions/*.jsonl")
foreach(SESSION_FILE IN LISTS SESSION_FILES)
  file(READ "${SESSION_FILE}" SESSION_CONTENT)
  foreach(PRIVATE_AGENT_TEXT "SELECTED_PRIMARY_CODER_CANARY" "coder")
    string(FIND "${SESSION_CONTENT}" "${PRIVATE_AGENT_TEXT}" PRIVATE_AGENT_INDEX)
    if(NOT PRIVATE_AGENT_INDEX EQUAL -1)
      message(FATAL_ERROR "selected primary identity was persisted in session history: ${PRIVATE_AGENT_TEXT}\n${SESSION_CONTENT}")
    endif()
  endforeach()
endforeach()

ava_cli_expect_contains("${AVA_OUTPUT}"
        "headless active prompt complete")

ava_cli_expect_contains("${PROVIDER_REQUEST}"
        "hello positional prompt"
        "workspace positional context should remain"
        "SELECTED_PRIMARY_CODER_CANARY"
        "\"name\":\"read_file\""
        "\"name\":\"grep\"")

foreach(FORBIDDEN
        "TASK_ONLY_CANARY"
        "\"name\":\"write_file\""
        "\"name\":\"bash\""
        "\"name\":\"task\""
        "\"name\":\"todowrite\"")
  string(FIND "${PROVIDER_REQUEST}" "${FORBIDDEN}" FORBIDDEN_INDEX)
  if(NOT FORBIDDEN_INDEX EQUAL -1)
    message(FATAL_ERROR "fake provider request unexpectedly contained ${FORBIDDEN}\nrequest:\n${PROVIDER_REQUEST}")
  endif()
endforeach()

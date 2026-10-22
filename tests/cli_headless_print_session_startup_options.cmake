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
set(AVA_TIMEOUT_45 45)
set(AVA_POLL_200 200)
if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT})
  set(AVA_DEBUG_SECONDS 3600)
  if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS} AND "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}" MATCHES "^[1-9][0-9]*$")
    set(AVA_DEBUG_SECONDS "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}")
  endif()
  set(AVA_TIMEOUT_45 "${AVA_DEBUG_SECONDS}")
  math(EXPR AVA_POLL_200 "${AVA_DEBUG_SECONDS} * 20")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cli_test_common.cmake")
ava_cli_timeouts(45)
ava_cli_provider_paths()

get_filename_component(TEST_ROOT "${AVA_CLI_TEST_ROOT}" ABSOLUTE)
set(WORKSPACE "${TEST_ROOT}/workspace")
set(HOME_DIR "${TEST_ROOT}/home")
set(CONFIG_DIR "${TEST_ROOT}/config")
set(STATE_DIR "${TEST_ROOT}/state")
set(DATA_DIR "${TEST_ROOT}/data")
set(CUSTOM_SESSIONS "${TEST_ROOT}/custom-sessions")
set(DRIVER_FILE "${TEST_ROOT}/driver.sh")
set(SOURCE_SESSION_ID_FILE "${TEST_ROOT}/source-session-id")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${WORKSPACE}" "${HOME_DIR}" "${CONFIG_DIR}/ava" "${STATE_DIR}" "${DATA_DIR}")
ava_cli_isolated_env(AVA_CLI_ENV "${TEST_ROOT}")
ava_cli_shell_env(AVA_CLI_ENV_SHELL ${AVA_CLI_ENV})
ava_cli_driver_support(AVA_CLI_DRIVER_SUPPORT "${TEST_ROOT}")
file(WRITE "${WORKSPACE}/AGENTS.md" "workspace startup option context should remain\n")
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
"run_ava() {\n"
"  label=\"$1\"\n"
"  shift\n"
"  ava_out=\"${TEST_ROOT}/ava-$label.out\"\n"
"  ava_err=\"${TEST_ROOT}/ava-$label.err\"\n"
"  rm -f \"$ava_out\" \"$ava_err\"\n"
"  fake_provider_start \"${TEST_ROOT}\" \"provider-$label\" 0 text unused || exit 1\n"
"  ava_run env ${AVA_CLI_ENV_SHELL} MOONSHOT_API_KEY=test-key MOONSHOT_BASE_URL=\"http://127.0.0.1:$FAKE_PROVIDER_PORT\" \"${AVA_EXE}\" \"$@\" > \"$ava_out\" 2> \"$ava_err\"\n"
"  ava_status=$?\n"
"  if [ \"$ava_status\" -ne 0 ]; then\n"
"    echo \"ava startup option prompt exited with $ava_status\" >&2\n"
"    cat \"$ava_out\" >&2 2>/dev/null || true\n"
"    cat \"$ava_err\" >&2 2>/dev/null || true\n"
"    exit \"$ava_status\"\n"
"  fi\n"
"  fake_provider_wait 0 ${AVA_TIMEOUT_45} || exit 1\n"
"  fake_provider_finish ${AVA_TIMEOUT_45} || exit 1\n"
"}\n"
"run_ava named --session-dir \"${CUSTOM_SESSIONS}\" --name \"startup named\" --output text \"named\" \"prompt\"\n"
"session_file=$(find \"${CUSTOM_SESSIONS}\" -name '*.jsonl' | sort | head -n 1)\n"
"if [ -z \"$session_file\" ]; then echo \"startup run did not create a custom session\" >&2; exit 1; fi\n"
"session_id=$(basename \"$session_file\" .jsonl)\n"
"printf '%s\\n' \"$session_id\" > \"${SOURCE_SESSION_ID_FILE}\"\n"
"run_ava sessionid --session-dir \"${CUSTOM_SESSIONS}\" --session-id \"$session_id\" --output text \"session id\" \"prompt\"\n"
"run_ava fork --session-dir \"${CUSTOM_SESSIONS}\" --fork \"$session_id\" --name \"fork startup\" --output text \"forked\" \"prompt\"\n"
"run_ava resume --session-dir \"${CUSTOM_SESSIONS}\" --resume --output text \"resume alias\" \"prompt\"\n")

execute_process(
  COMMAND ${AVA_CLI_DRIVER_COMMAND} /bin/sh "${DRIVER_FILE}"
  WORKING_DIRECTORY "${WORKSPACE}"
  OUTPUT_VARIABLE DRIVER_OUTPUT
  ERROR_VARIABLE DRIVER_ERROR
  RESULT_VARIABLE DRIVER_RESULT
  TIMEOUT ${AVA_TIMEOUT_45}
)

if(NOT DRIVER_RESULT EQUAL 0)
  message(FATAL_ERROR "headless startup options driver exited with ${DRIVER_RESULT}\nstdout:\n${DRIVER_OUTPUT}\nstderr:\n${DRIVER_ERROR}")
endif()

file(READ "${TEST_ROOT}/ava-named.out" NAMED_OUTPUT)
file(READ "${TEST_ROOT}/ava-named.err" NAMED_ERROR)
file(READ "${TEST_ROOT}/provider-named-requests.log" NAMED_REQUEST)
file(READ "${TEST_ROOT}/ava-sessionid.out" SESSION_ID_OUTPUT)
file(READ "${TEST_ROOT}/ava-sessionid.err" SESSION_ID_ERROR)
file(READ "${TEST_ROOT}/provider-sessionid-requests.log" SESSION_ID_REQUEST)
file(READ "${TEST_ROOT}/ava-fork.out" FORK_OUTPUT)
file(READ "${TEST_ROOT}/ava-fork.err" FORK_ERROR)
file(READ "${TEST_ROOT}/provider-fork-requests.log" FORK_REQUEST)
file(READ "${TEST_ROOT}/ava-resume.out" RESUME_OUTPUT)
file(READ "${TEST_ROOT}/ava-resume.err" RESUME_ERROR)
file(READ "${TEST_ROOT}/provider-resume-requests.log" RESUME_REQUEST)
file(READ "${SOURCE_SESSION_ID_FILE}" SOURCE_SESSION_ID)
string(STRIP "${SOURCE_SESSION_ID}" SOURCE_SESSION_ID)

foreach(OUTPUT IN ITEMS "${NAMED_OUTPUT}" "${SESSION_ID_OUTPUT}" "${FORK_OUTPUT}" "${RESUME_OUTPUT}")
  string(FIND "${OUTPUT}" "headless active prompt complete" NEEDLE_INDEX)
  if(NEEDLE_INDEX EQUAL -1)
    message(FATAL_ERROR "ava startup option output did not contain fake provider text\nnamed stdout:\n${NAMED_OUTPUT}\nnamed stderr:\n${NAMED_ERROR}\nsession-id stdout:\n${SESSION_ID_OUTPUT}\nsession-id stderr:\n${SESSION_ID_ERROR}\nfork stdout:\n${FORK_OUTPUT}\nfork stderr:\n${FORK_ERROR}\nresume stdout:\n${RESUME_OUTPUT}\nresume stderr:\n${RESUME_ERROR}")
  endif()
endforeach()

foreach(REQUEST IN ITEMS "${NAMED_REQUEST}" "${SESSION_ID_REQUEST}" "${FORK_REQUEST}" "${RESUME_REQUEST}")
  ava_cli_expect_contains("${REQUEST}" "workspace startup option context should remain" "prompt")
endforeach()

file(GLOB_RECURSE DEFAULT_SESSION_FILES "${STATE_DIR}/ava/sessions/*.jsonl")
if(DEFAULT_SESSION_FILES)
  message(FATAL_ERROR "--session-dir wrote sessions into the default state directory:\n${DEFAULT_SESSION_FILES}")
endif()

file(GLOB_RECURSE SESSION_FILES "${CUSTOM_SESSIONS}/*.jsonl")
list(LENGTH SESSION_FILES SESSION_FILE_COUNT)
if(NOT SESSION_FILE_COUNT EQUAL 2)
  message(FATAL_ERROR "--session-dir/--fork expected two custom session files, found ${SESSION_FILE_COUNT}:\n${SESSION_FILES}")
endif()

set(SESSION_TEXT "")
foreach(SESSION_FILE IN LISTS SESSION_FILES)
  file(READ "${SESSION_FILE}" CONTENTS)
  string(APPEND SESSION_TEXT "${CONTENTS}\n")
endforeach()

ava_cli_expect_contains("${SESSION_TEXT}"
        "\"name\":\"startup named\""
        "\"name\":\"fork startup\""
        "\"branch_origin\":\"fork\""
        "\"actor\":\"cli\""
        "\"parent_session_id\":\"${SOURCE_SESSION_ID}\""
        "\"source_session_id\":\"${SOURCE_SESSION_ID}\"")

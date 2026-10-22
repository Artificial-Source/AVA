if(NOT DEFINED AVA_PLUGIN_RUNNER_SCANNER OR NOT DEFINED AVA_PLUGIN_RUNNER_NEGATIVE_ROOT)
  message(FATAL_ERROR "plugin runner scanner self-test requires scanner and scratch paths")
endif()

# Own a per-invocation child: concurrent CTest processes must never remove a peer's fixture.
string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef invocation)
set(root "${AVA_PLUGIN_RUNNER_NEGATIVE_ROOT}/${invocation}")
file(MAKE_DIRECTORY "${root}")
set(cases
  "fork|::fork()" "vfork|vfork()" "exec|execve(nullptr)"
  "spawn|posix_spawn(nullptr)" "spawnp|posix_spawnp(nullptr)"
  "popen|popen(nullptr)" "system|system(nullptr)" "setpgid|setpgid(0, 0)"
  "kill|::kill(0, 0)" "killpg|killpg(0, 0)"
  "wait|wait(nullptr)" "waitpid|waitpid(0, nullptr, 0)" "waitid|waitid(0)"
  "qualified_wait|::wait(nullptr)"
  "fcntl|fcntl(0)" "pipe|pipe(nullptr)" "pipe2|pipe2(nullptr, 0)"
  "dup|dup(0)" "dup2|dup2(0, 1)" "dup3|dup3(0, 1, 0)"
  "poll|poll(nullptr, 0, 0)" "ppoll|ppoll(nullptr, 0, nullptr, nullptr)"
  "include|#include <sys/wait.h>" "pid|pid_t child"
  "fd|stdin_fd_" "signal|SIGTERM" "sigaction|sigaction(0)"
  "signal_call|signal(0)" "ignore|ScopedSignalIgnore" "unique_fd|UniqueFd"
  "pollfd|pollfd descriptor" "status|child_status_" "group|can_signal_group_")
foreach(case IN LISTS cases)
  string(REPLACE "|" ";" fields "${case}")
  list(GET fields 0 name)
  list(GET fields 1 source)
  set(directory "${root}/${name}")
  file(MAKE_DIRECTORY "${directory}")
  file(WRITE "${directory}/runner_negative.cpp" "${source}\n")
  file(WRITE "${directory}/CMakeLists.txt" "# scanner rejection fixture\n")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DAVA_PLUGIN_RUNNER_DIR=${directory}"
      "-DAVA_PLUGIN_CMAKE=${directory}/CMakeLists.txt" -P "${AVA_PLUGIN_RUNNER_SCANNER}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "forbidden local process authority")
    file(REMOVE_RECURSE "${root}")
    message(FATAL_ERROR "plugin runner scanner must reject ${name} for process authority: ${result} ${output}${error}")
  endif()
endforeach()

# A negative scanner that rejects every source is not a useful contract.
set(directory "${root}/allowed")
file(MAKE_DIRECTORY "${directory}")
file(WRITE "${directory}/runner.h" [=[
optional<ava::process::ProcessScopeV1> process_scope
ava::process::ProcessHandle process_handle_
ava::process::PipeEndpoint standard_input_
AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
]=])
file(WRITE "${directory}/runner_process.cpp" [=[
ProcessRoleV1::Plugin
make_plugin_environment_v1
.spawn(
StreamModeV1::Capture
->operation()
CleanupStateV1::Complete
request_kill(); skill(); myfork(); mypoll();
supervisor.wait(handle, deadline); supervisor->wait(handle, deadline);
MyUniqueFd scoped_child_status_ SIGTERM_suffix prefix_can_signal_group_
]=])
file(WRITE "${directory}/CMakeLists.txt" "target_link_libraries(fixture AVA::process)\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DAVA_PLUGIN_RUNNER_DIR=${directory}"
    "-DAVA_PLUGIN_CMAKE=${directory}/CMakeLists.txt" -P "${AVA_PLUGIN_RUNNER_SCANNER}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
file(REMOVE_RECURSE "${root}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "plugin runner scanner rejected allowed identifier boundaries: ${output}${error}")
endif()

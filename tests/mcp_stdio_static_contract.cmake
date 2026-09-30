if(NOT DEFINED AVA_MCP_STDIO_DIR OR NOT DEFINED AVA_MCP_CMAKE)
  message(FATAL_ERROR "MCP stdio static contract requires AVA_MCP_STDIO_DIR and AVA_MCP_CMAKE")
endif()

file(GLOB AVA_MCP_STDIO_FILES
  "${AVA_MCP_STDIO_DIR}/stdio_client*.h"
  "${AVA_MCP_STDIO_DIR}/stdio_client*.cpp")
if(NOT AVA_MCP_STDIO_FILES)
  message(FATAL_ERROR "MCP stdio static contract found no production files")
endif()

foreach(AVA_MCP_STDIO_FILE IN LISTS AVA_MCP_STDIO_FILES)
  file(READ "${AVA_MCP_STDIO_FILE}" AVA_MCP_STDIO_TEXT)
  foreach(AVA_FORBIDDEN_PATTERN IN ITEMS
      "(^|[^A-Za-z0-9_])(fork|vfork)[ \t\r\n]*\\("
      "(^|[^A-Za-z0-9_])exec(v|ve|vp|vpe|veat|l|le|lp)?[ \t\r\n]*\\("
      "(^|[^A-Za-z0-9_])(posix_spawn|popen|system)[ \t\r\n]*\\("
      "(^|[^A-Za-z0-9_])setpgid[ \t\r\n]*\\("
      "(^|[^A-Za-z0-9_])kill(pg)?[ \t\r\n]*\\("
      "(^|[^A-Za-z0-9_])wait(pid|id)[ \t\r\n]*\\("
      "(^|[^A-Za-z0-9_])fcntl[ \t\r\n]*\\("
      "(^|[^A-Za-z0-9_])(pipe|pipe2)[ \t\r\n]*\\("
      "(^|[^A-Za-z0-9_])(dup|dup2)[ \t\r\n]*\\("
      "(^|[^A-Za-z0-9_])poll[ \t\r\n]*\\("
      "#[ \t]*include[ \t]*[<\"][^>\"]*(poll|signal|spawn|sys/wait|unistd|fcntl)\\.h[>\"]"
      "(^|[^A-Za-z0-9_])pid_t([^A-Za-z0-9_]|$)"
      "(^|[^A-Za-z0-9_])(stdin|stdout|stderr)_fd_([^A-Za-z0-9_]|$)"
      "ScopedSignalIgnore"
      "UniqueFd"
      "SIG(PIPE|TERM|KILL|CHLD|_IGN)"
      "(^|[^A-Za-z0-9_])(sigaction|signal)[ \t\r\n]*\\("
      "child_status_"
      "can_signal_group_")
    string(REGEX MATCH "${AVA_FORBIDDEN_PATTERN}" AVA_FORBIDDEN_MATCH "${AVA_MCP_STDIO_TEXT}")
    if(AVA_FORBIDDEN_MATCH)
      message(FATAL_ERROR "MCP stdio client retains forbidden local process authority in ${AVA_MCP_STDIO_FILE}: ${AVA_FORBIDDEN_MATCH}")
    endif()
  endforeach()
endforeach()

file(READ "${AVA_MCP_STDIO_DIR}/stdio_client.h" AVA_MCP_STDIO_HEADER)
foreach(AVA_REQUIRED_HEADER_PATTERN IN ITEMS
    "optional<ava::process::ProcessScopeV1> process_scope"
    "ava::process::ProcessHandle process_handle_"
    "ava::process::PipeEndpoint standard_input_"
    "AVA_DEBUG_PRINT_MEMBERS_OPT_OUT")
  string(FIND "${AVA_MCP_STDIO_HEADER}" "${AVA_REQUIRED_HEADER_PATTERN}" AVA_REQUIRED_OFFSET)
  if(AVA_REQUIRED_OFFSET EQUAL -1)
    message(FATAL_ERROR "MCP stdio client header is missing required supervised ownership: ${AVA_REQUIRED_HEADER_PATTERN}")
  endif()
endforeach()

file(READ "${AVA_MCP_STDIO_DIR}/stdio_client.cpp" AVA_MCP_STDIO_PROCESS)
foreach(AVA_REQUIRED_PROCESS_PATTERN IN ITEMS
    "ProcessRoleV1::Mcp"
    "make_mcp_environment_v1"
    ".spawn("
    "StreamModeV1::Capture"
    "->operation()"
    "CleanupStateV1::Complete")
  string(FIND "${AVA_MCP_STDIO_PROCESS}" "${AVA_REQUIRED_PROCESS_PATTERN}" AVA_REQUIRED_OFFSET)
  if(AVA_REQUIRED_OFFSET EQUAL -1)
    message(FATAL_ERROR "MCP stdio client process implementation is missing: ${AVA_REQUIRED_PROCESS_PATTERN}")
  endif()
endforeach()

file(READ "${AVA_MCP_CMAKE}" AVA_MCP_CMAKE_TEXT)
string(FIND "${AVA_MCP_CMAKE_TEXT}" "AVA::process" AVA_PROCESS_LINK_OFFSET)
if(AVA_PROCESS_LINK_OFFSET EQUAL -1)
  message(FATAL_ERROR "AVA::mcp must link to AVA::process")
endif()

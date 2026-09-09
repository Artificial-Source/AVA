#include "sys.h"
// clang-format off
#include "debug.h"                      // Must be included BEFORE print_members_on.h.
#include "print_members_on.h"
// clang-format on

NAMESPACE_DEBUG_CHANNELS_START
Channel ava("AVA");
Channel agent("AGENT");
Channel app("APP");
Channel rpc("RPC");
Channel runtime("RUNTIME");
Channel config("CONFIG");
Channel context("CONTEXT");
Channel avacore("AVACORE");
Channel json("JSON");
Channel lsp("LSP");
Channel mcp("MCP");
Channel permissions("PERMISSIONS");
Channel plugin("PLUGIN");
Channel provider("PROVIDER");
Channel session("SESSION");
Channel tools("TOOLS");
Channel tui("TUI");
Channel terminal("TERMINAL");
libcwd::FatalChannel const& coredump{libcwd::channels::dc::core};
NAMESPACE_DEBUG_CHANNELS_END

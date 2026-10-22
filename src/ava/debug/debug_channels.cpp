#include "sys.h"
#include "debug.h"

#ifdef CWDEBUG
NAMESPACE_DEBUG_CHANNELS_START
Channel ava("AVA");
Channel avacore("AVACORE");
Channel agent("AGENT");
Channel app("APP");
Channel config("CONFIG");
Channel context("CONTEXT");
Channel frontend("FRONTEND");
Channel json("JSON");
Channel lsp("LSP");
Channel mcp("MCP");
Channel permissions("PERMISSIONS");
Channel plugin("PLUGIN");
Channel provider("PROVIDER");
Channel rpc("RPC");
Channel runtime("RUNTIME");
Channel session("SESSION");
Channel tools("TOOLS");
Channel tui("TUI");
Channel terminal("TERMINAL");
NAMESPACE_DEBUG_CHANNELS_END
#endif

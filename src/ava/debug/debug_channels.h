#ifdef CWDEBUG

#include "NAMESPACE_DEBUG.h"
#include "libcwd/Channel.h"

namespace LIBCWD_DEBUG_CHANNELS::dc {
using libcwd::Channel;
extern Channel ava;
extern Channel agent;
extern Channel app;
extern Channel rpc;
extern Channel runtime;
extern Channel config;
extern Channel context;
extern Channel avacore;
extern Channel json;
extern Channel lsp;
extern Channel mcp;
extern Channel permissions;
extern Channel plugin;
extern Channel provider;
extern Channel session;
extern Channel tools;
extern Channel tui;
extern Channel terminal;
} // namespace LIBCWD_DEBUG_CHANNELS::dc

#endif

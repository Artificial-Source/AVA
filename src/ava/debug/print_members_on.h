/*=****************************************************************************
 * Convenience marocs                                                         *
 ******************************************************************************/

#ifdef CWDEBUG
#include <iostream>
#include <libcwd/type_info.h>
#endif

// The idea behind passing a prefix to print_members is so that you can call
// print_members(os, prefix) from within another print_members to append the
// members (start with an optional leading ", " if anything was already printed
// before that point).
//
#define AVA_PRINT_ON_MEMBERS \
  void print_on(std::ostream& os) const \
  { \
    os << '{'; \
    print_members(os, ""); \
    os << '}'; \
  } \
  void print_members(std::ostream& os, char const* prefix) const;

// To add a print_on member to a class with a single base class.
//
#define AVA_PRINT_ON_BASE_MEMBERS(base_class) \
  void print_on(std::ostream& os) const \
  { \
    os << '{'; \
    base_class::print_members(os, #base_class ":{"); \
    os << "}, "; \
    print_members(os, ""); \
    os << '}'; \
  } \
  void print_members(std::ostream& os, char const* prefix) const;

// If a type is pure virtual then it must have been passed as reference,
// just print it as such.
//
#define AVA_PURE_VIRTUAL_PRINT_ON_MEMBERS \
  void print_on(std::ostream& os) const \
  { \
    os << libcwd::type_info_of(*this).demangled_name() << '@' << (void*)this; \
  }

#define AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

#ifndef CWDEBUG
// AVA_DEBUG_MEMBERS only declares print_on in Debug mode.
#define AVA_DEBUG_PRINT_MEMBERS_ON
#define AVA_DEBUG_PRINT_MEMBERS_ON_BASE(base_class)
#define AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS
#else
#define AVA_DEBUG_PRINT_MEMBERS_ON AVA_PRINT_ON_MEMBERS
#define AVA_DEBUG_PRINT_MEMBERS_ON_BASE(base_class) AVA_PRINT_ON_BASE_MEMBERS(base_class)
#define AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS AVA_PURE_VIRTUAL_PRINT_ON_MEMBERS

// Forward declarations.
namespace libcwd { class Channel; class FatalChannel; }

#include "NAMESPACE_DEBUG.h"            // NAMESPACE_DEBUG_CHANNELS_START/END

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
extern libcwd::FatalChannel const& coredump;
} // namespace LIBCWD_DEBUG_CHANNELS::dc

#endif // CWDEBUG

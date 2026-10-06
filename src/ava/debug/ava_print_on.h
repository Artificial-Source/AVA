#pragma once

#include "utils/has_print_on.h"

namespace ava {

// Print objects from namespace ava, using ADL, if they have a print_on member.
using utils::has_print_on::operator<<;

namespace agent {
using utils::has_print_on::operator<<;
namespace tool_dispatch {
using utils::has_print_on::operator<<;
} //  namespace tool_dispatch
} // namespace agent

namespace app {
using utils::has_print_on::operator<<;
namespace rpc {
using utils::has_print_on::operator<<;
namespace detail {
using utils::has_print_on::operator<<;
} //  namespace detail
} // namespace rpc
namespace runtime {
using utils::has_print_on::operator<<;
} // namespace runtime
namespace frontend {
using utils::has_print_on::operator<<;
} // namespace runtime
} // namespace app

namespace command {
using utils::has_print_on::operator<<;
} // namespace command

namespace config {
using utils::has_print_on::operator<<;
} // namespace config

namespace containment {
using utils::has_print_on::operator<<;
} // namespace containment

namespace context {
using utils::has_print_on::operator<<;
} // namespace context

namespace core {
using utils::has_print_on::operator<<;
namespace json {
using utils::has_print_on::operator<<;
} // namespace json
} // namespace core

namespace diagnostics {
using utils::has_print_on::operator<<;
} // namespace diagnostics

namespace event {
using utils::has_print_on::operator<<;
} // namespace event

namespace lsp {
using utils::has_print_on::operator<<;
} // namespace lsp

namespace mcp {
using utils::has_print_on::operator<<;
} // namespace mcp

namespace observability {
using utils::has_print_on::operator<<;
} // namespace observability

namespace permissions {
using utils::has_print_on::operator<<;
namespace permission_rules_internal {
using utils::has_print_on::operator<<;
} // namespace permission_rules_internal
} // namespace permissions

namespace plugin {
using utils::has_print_on::operator<<;
} // namespace plugin

namespace provider {
using utils::has_print_on::operator<<;
} // namespace provider

namespace session {
using utils::has_print_on::operator<<;
} // namespace session

namespace tools {
using utils::has_print_on::operator<<;
} // namespace tools

namespace tui {
using utils::has_print_on::operator<<;
namespace detail {
using utils::has_print_on::operator<<;
} // namespace detail
namespace runtime_views {
using utils::has_print_on::operator<<;
} // namespace runtime_views
namespace terminal {
using utils::has_print_on::operator<<;
} // namespace terminal
} // namespace tui

} // namespace ava

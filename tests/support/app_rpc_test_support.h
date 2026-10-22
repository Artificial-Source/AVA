#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace ava::tests {

class ThreadSafeStringBuf;

// Borrows the first complete, valid response record with this exact top-level
// string id; nested ids/events and a final unterminated fragment never match.
// Keep jsonl alive and unchanged while using the returned view.
[[nodiscard]] std::optional<std::string_view> rpc_response_line(std::string_view jsonl, std::string_view id);
// Non-consuming terminal-record wait; does not imply success. Inspect the
// correlated rpc_response_line after this returns and before closing RPC stdin.
[[nodiscard]] bool wait_for_rpc_response(ThreadSafeStringBuf& output, std::string_view id, std::chrono::milliseconds timeout);

// Legacy first-field extraction at any nesting level, delegating to the shared
// extract_json_string_field convention (raw, not JSON-unescaped). Use parsed
// fields on rpc_response_line for correlated response assertions instead.
std::optional<std::string> rpc_string_field_from_output(std::string_view jsonl, std::string_view field);

}  // namespace ava::tests

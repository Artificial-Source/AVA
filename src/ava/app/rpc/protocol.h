#pragma once

#include "ava/app/rpc/catalog.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app {

struct RpcImageUpload
{
  std::string type;
  ava::core::NoLogString data_base64;
  std::string mime_type;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct RpcCommand
{
  std::string id;
  std::string type;
  std::optional<long long> protocol_version;
  std::optional<ava::core::NoLogString> message;
  std::optional<std::string> session_id;
  std::optional<std::string> job_id;
  std::optional<long long> timeout_ms;
  std::optional<std::string> provider;
  std::optional<std::string> model;
  std::optional<ava::core::NoLogString> instructions;
  std::optional<std::string> reasoning_level;
  std::optional<long long> reasoning_budget_tokens;
  std::optional<std::string> reasoning_display;
  std::optional<std::string> request_id;
  std::optional<std::string> correlation_id;
  std::optional<std::string> grant_id;
  std::optional<std::string> rule_id;
  std::optional<std::string> decision;
  std::optional<std::string> action;
  std::optional<std::string> operation;
  std::optional<std::string> scope;
  std::optional<std::string> mode;
  std::optional<ava::core::NoLogString> reason;
  std::optional<ava::core::NoLogString> session_name;
  std::optional<std::vector<ava::core::NoLogString>> labels;
  std::optional<std::string> branch_from_entry_id;
  std::optional<std::string> branch_root_entry_id;
  std::optional<std::string> branch_tip_entry_id;
  std::optional<ava::core::NoLogString> summary;
  std::optional<ava::core::NoLogString> answer;
  std::optional<ava::core::NoLogString> selected;
  std::optional<std::vector<ava::core::NoLogString>> selected_options;
  std::optional<std::vector<ava::core::NoLogString>> attachments;
  std::optional<std::vector<RpcImageUpload>> images;
  std::optional<std::string> plugin_id;
  std::optional<std::string> name;
  std::optional<ava::core::NoLogString> arguments;
  std::optional<ava::core::NoLogString> command_arguments;
  std::optional<std::string> server_id;
  std::optional<ava::core::NoLogString> output_path;
  std::optional<ava::core::NoLogString> path;
  std::optional<ava::core::NoLogString> target_path;
  std::optional<ava::core::NoLogString> command;
  std::optional<std::string> tool_name;
  std::optional<std::string> command_recipe_key;
  std::optional<ava::core::NoLogString> recipe_display;
  std::optional<bool> critical_acknowledged;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<RpcCommand> parse_rpc_command_line(std::string_view line);
[[nodiscard]] std::string serialize_rpc_success_jsonl(std::string_view id, std::string_view result_json);
[[nodiscard]] std::string serialize_rpc_error_jsonl(std::string_view id, ava::core::Error const& error);

}  // namespace ava::app

namespace ava::app::rpc {

inline constexpr std::size_t kMaxRpcLineBytes = 32 * 1024 * 1024;
inline constexpr std::size_t kMaxRpcNestingDepth = 64;
inline constexpr std::size_t kMaxRpcMessagesResponseBytes = 1024 * 1024;
inline constexpr std::size_t kMaxRpcMessagesEntries = 1000;
inline constexpr std::size_t kMaxRpcQueuedMessages = 64;
inline constexpr std::size_t kMaxRpcQueuedMessageBytes = 64 * 1024;
inline constexpr std::size_t kMaxRpcQueueEventMessageBytes = 512;
inline constexpr std::size_t kMaxRpcIdentifierBytes = 256;
inline constexpr std::size_t kMaxRpcReasonBytes = 1024;
inline constexpr std::size_t kMaxRpcRuleCommandBytes = 8192;
inline constexpr std::size_t kMaxRpcRulePathBytes = 8192;
inline constexpr std::size_t kMaxRpcExportPathBytes = 8192;
inline constexpr std::size_t kMaxRpcSessionNameBytes = 256;
inline constexpr std::size_t kMaxRpcSessionLabels = 32;
inline constexpr std::size_t kMaxRpcSessionLabelBytes = 64;
inline constexpr std::size_t kMaxRpcEntryIdBytes = 256;
inline constexpr std::size_t kMaxRpcBranchSummaryBytes = 8192;
inline constexpr std::size_t kMaxRpcQuestionAnswerBytes = 8192;
inline constexpr std::size_t kMaxRpcQuestionSelectedOptions = 64;
inline constexpr std::size_t kMaxRpcPromptAttachments = 16;
inline constexpr std::size_t kMaxRpcPromptAttachmentPathBytes = 8192;
inline constexpr std::size_t kMaxRpcPromptImageDataBase64Bytes = ((20 * 1024 * 1024 + 2) / 3) * 4;
inline constexpr std::size_t kMaxRpcPromptImageMimeTypeBytes = 128;
inline constexpr long long kRpcProtocolVersion = kRpcProtocolVersions.protocol;

[[nodiscard]] ava::core::Error invalid_rpc(std::string message);

[[nodiscard]] ava::core::VoidResult validate_protocol_version(RpcCommand const& command);
[[nodiscard]] std::string rpc_protocol_result_json();
[[nodiscard]] std::string parse_error_response_id(std::string_view line);
[[nodiscard]] ava::core::Result<bool> read_rpc_line_bounded(std::istream& in, std::string& line);

}  // namespace ava::app::rpc

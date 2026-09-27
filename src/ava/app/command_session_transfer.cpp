#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/command_session_support_internal.h"
#include "ava/app/command_sessions.h"
#include "ava/app/command_tools.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/tools/file_tools.h"
#include "ava/session/export.h"
#include "ava/session/record.h"
#include "ava/session/validation.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::app {
using session_command_support::load_runtime_entries;
using session_command_support::lower_ascii;

namespace {

std::function<void()> after_session_import_open_for_test;

class ScopedImportFd
{
 public:
  explicit ScopedImportFd(int fd) noexcept : fd_(fd) { }
  ScopedImportFd(ScopedImportFd const&) = delete;
  ScopedImportFd& operator=(ScopedImportFd const&) = delete;
  ~ScopedImportFd()
  {
    if (fd_ >= 0)
      static_cast<void>(::close(fd_));
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] int close_checked() noexcept
  {
    if (fd_ < 0)
      return 0;
    int const fd = std::exchange(fd_, -1);
    return ::close(fd) == 0 ? 0 : errno;
  }

 private:
  int fd_ = -1;
};

enum class ExportFormat
{
  Markdown,
  Html,
  Jsonl,
};

struct ExportCommandArguments
{
  ExportFormat format = ExportFormat::Markdown;
  std::string path;
};

struct JsonlAttachmentSummary
{
  std::size_t count = 0;
  std::string first_entry_id;
  std::string first_storage_path;
};

std::string export_format_text(ExportFormat format)
{
  switch (format)
  {
    case ExportFormat::Markdown:
      return "markdown";
    case ExportFormat::Html:
      return "html";
    case ExportFormat::Jsonl:
      return "jsonl";
  }
  return "markdown";
}

std::string_view trim_ascii_view(std::string_view text)
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
  return text;
}

std::string_view first_token(std::string_view text)
{
  text = trim_ascii_view(text);
  auto const end = text.find_first_of(" \t\r\n");
  return text.substr(0, end == std::string_view::npos ? text.size() : end);
}

std::string_view after_first_token(std::string_view text)
{
  text = trim_ascii_view(text);
  auto const end = text.find_first_of(" \t\r\n");
  if (end == std::string_view::npos)
    return {};
  return trim_ascii_view(text.substr(end + 1));
}

bool path_looks_html(std::string_view path)
{
  auto const lowered = lower_ascii(path);
  return lowered.ends_with(".html") || lowered.ends_with(".htm");
}

bool path_looks_jsonl(std::string_view path)
{
  auto const lowered = lower_ascii(path);
  return lowered.ends_with(".jsonl");
}

ava::core::Result<std::string> format_session_jsonl(std::vector<ava::session::SessionEntry> const& entries)
{
  return ava::session::format_session_portable_jsonl_checked(entries);
}

bool json_bool_field_is_true(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  return start && object.substr(*start, 4) == "true";
}

JsonlAttachmentSummary summarize_non_redacted_jsonl_attachments(std::vector<ava::session::SessionEntry> const& entries)
{
  JsonlAttachmentSummary summary;
  for (auto const& entry : entries)
  {
    if (entry.type != ava::session::EntryType::UserMessage)
      continue;
    auto const sanitized = ava::session::sanitized_message_data_json(entry.data_json);
    for (auto const& attachment : ava::core::json::objects_in_array_field(sanitized, "attachments"))
    {
      if (ava::core::json::string_field(attachment, "type").value_or("") != "image")
        continue;
      if (json_bool_field_is_true(attachment, "redacted"))
        continue;
      auto const storage_path = ava::core::json::string_field(attachment, "storage_path").value_or("");
      if (storage_path.empty())
        continue;
      ++summary.count;
      if (summary.first_entry_id.empty())
      {
        summary.first_entry_id = entry.id;
        summary.first_storage_path = storage_path;
      }
    }
  }
  return summary;
}

ava::core::VoidResult validate_jsonl_import_attachments(std::vector<ava::session::SessionEntry> const& entries, std::filesystem::path const& path)
{
  auto const summary = summarize_non_redacted_jsonl_attachments(entries);
  if (summary.count == 0)
    return {};

  auto error =
      ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session import has non-redacted image attachment metadata without attachment bytes");
  error.with_context("path", path.string());
  error.with_context("attachments", std::to_string(summary.count));
  error.with_context("first_entry_id", summary.first_entry_id);
  error.with_context("first_storage_path", summary.first_storage_path);
  error.with_context("remediation", "import a portable redacted JSONL export or provide an attachment-aware archive");
  return std::unexpected(std::move(error));
}

bool looks_like_pi_session_header(std::string_view line)
{
  if (!ava::core::json::is_valid_object(line))
    return false;
  auto const type = ava::core::json::string_field(line, "type");
  return type && *type == "session" && ava::core::json::string_field(line, "id") && ava::core::json::string_field(line, "timestamp") &&
         ava::core::json::string_field(line, "cwd");
}

ava::core::Error unsupported_pi_session_import_error(std::filesystem::path const& path)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Session, "Pi session import is not supported yet");
  error.with_context("path", path.string());
  return error;
}

ava::core::Result<ExportCommandArguments> parse_export_command_arguments(std::string_view argument)
{
  argument = trim_ascii_view(argument);
  ExportCommandArguments parsed;
  if (argument.empty())
    return parsed;

  auto const token = lower_ascii(first_token(argument));
  if (token == "markdown" || token == "md")
  {
    parsed.format = ExportFormat::Markdown;
    parsed.path = std::string(after_first_token(argument));
    return parsed;
  }
  if (token == "html")
  {
    parsed.format = ExportFormat::Html;
    parsed.path = std::string(after_first_token(argument));
    return parsed;
  }
  if (token == "jsonl" || token == "raw")
  {
    parsed.format = ExportFormat::Jsonl;
    parsed.path = std::string(after_first_token(argument));
    return parsed;
  }
  if (token == "--format")
  {
    auto const format_token = lower_ascii(first_token(after_first_token(argument)));
    if (format_token == "markdown" || format_token == "md")
    {
      parsed.format = ExportFormat::Markdown;
    }
    else if (format_token == "html")
    {
      parsed.format = ExportFormat::Html;
    }
    else if (format_token == "jsonl" || format_token == "raw")
    {
      parsed.format = ExportFormat::Jsonl;
    }
    else
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "unsupported export format");
      error.with_context("format", std::string(format_token));
      error.with_context("supported", "markdown, html, jsonl");
      return std::unexpected(std::move(error));
    }
    parsed.path = std::string(after_first_token(after_first_token(argument)));
    return parsed;
  }

  parsed.path = std::string(argument);
  parsed.format = path_looks_html(argument) ? ExportFormat::Html : (path_looks_jsonl(argument) ? ExportFormat::Jsonl : ExportFormat::Markdown);
  return parsed;
}

std::filesystem::path resolve_export_path(runtime::session_ts const& unlocked_session, std::string_view path)
{
  auto resolved = std::filesystem::path(std::string(path));
  if (resolved.is_relative())
  {
    auto const current_dir = runtime::session_ts::crat(unlocked_session)->current_dir();
    resolved = current_dir / resolved;
  }
  return resolved.lexically_normal();
}

void attach_created_session_cleanup_context(ava::session::SessionStore const& store, ava::session::SessionLease const& lease, ava::core::Error& error)
{
  auto removed = store.remove_created_file(lease);
  if (!removed)
  {
    error.with_context("rollback_path", store.session_path().string());
    error.with_context("rollback_cause", removed.error().format());
  }
}

ava::core::Result<std::vector<ava::session::SessionEntry>> load_import_session_entries(std::filesystem::path const& path)
{
  ScopedImportFd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (file.get() < 0)
  {
    int const error_number = errno;
    auto const category = error_number == ENOENT ? ava::core::ErrorCategory::NotFound
                                                 : (error_number == ELOOP ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io);
    auto error = ava::core::Error(category, error_number == ENOENT ? "session import file not found"
                                                                   : (error_number == ELOOP ? "session import path must be a regular file and not a symlink"
                                                                                            : "failed to securely open session import file"));
    error.with_context("path", path.string()).with_context("cause", std::strerror(error_number));
    return std::unexpected(std::move(error));
  }

  struct stat status{};
  if (fstat(file.get(), &status) != 0)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect opened session import file");
    error.with_context("path", path.string()).with_context("cause", std::strerror(errno));
    return std::unexpected(std::move(error));
  }
  if (!S_ISREG(status.st_mode))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "session import path must be a regular file and not a symlink");
    error.with_context("path", path.string()).with_context("file_type", S_ISFIFO(status.st_mode) ? "fifo" : "other");
    return std::unexpected(std::move(error));
  }
  if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > kMaxSessionImportFileBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session import file exceeds byte limit");
    error.with_context("path", path.string())
        .with_context("max_file_bytes", std::to_string(kMaxSessionImportFileBytes))
        .with_context("remediation", "reduce or split the JSONL import before retrying");
    return std::unexpected(std::move(error));
  }

  if (after_session_import_open_for_test)
  {
    try
    {
      auto hook = after_session_import_open_for_test;
      hook();
    }
    catch (...)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "session import post-open test hook failed");
      error.with_context("path", path.string());
      return std::unexpected(std::move(error));
    }
  }

  std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size())
  {
    ssize_t count = 0;
    do
    {
      count = ::pread(file.get(), bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
    } while (count < 0 && errno == EINTR);
    if (count <= 0)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed while reading opened session import file");
      error.with_context("path", path.string()).with_context("cause", std::strerror(count < 0 ? errno : EIO));
      return std::unexpected(std::move(error));
    }
    offset += static_cast<std::size_t>(count);
  }
  int const close_error = file.close_checked();
  if (close_error != 0)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to close session import file");
    error.with_context("path", path.string()).with_context("cause", std::strerror(close_error));
    return std::unexpected(std::move(error));
  }

  std::vector<ava::session::SessionEntry> entries;
  entries.reserve(std::min<std::size_t>(kMaxSessionImportEntries, 256));
  auto consume_line = [&](std::string_view line) -> ava::core::VoidResult {
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    if (line.size() >= kMaxSessionImportLineBytes)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session import line exceeds byte limit");
      error.with_context("path", path.string())
          .with_context("max_line_bytes", std::to_string(kMaxSessionImportLineBytes - 1))
          .with_context("remediation", "reduce the oversized JSONL record before retrying");
      return std::unexpected(std::move(error));
    }
    if (entries.size() >= kMaxSessionImportEntries)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session import entry count exceeds limit");
      error.with_context("path", path.string())
          .with_context("max_entries", std::to_string(kMaxSessionImportEntries))
          .with_context("remediation", "split the JSONL history before retrying");
      return std::unexpected(std::move(error));
    }
    if (entries.empty() && looks_like_pi_session_header(line))
      return std::unexpected(unsupported_pi_session_import_error(path));
    auto entry = ava::session::parse_session_entry_line(line, path);
    if (!entry)
      return std::unexpected(std::move(entry.error()));
    entries.push_back(std::move(*entry));
    return {};
  };

  std::size_t line_start = 0;
  while (line_start < bytes.size())
  {
    auto const newline = bytes.find('\n', line_start);
    auto const line_end = newline == std::string::npos ? bytes.size() : newline;
    if (auto consumed = consume_line(std::string_view(bytes).substr(line_start, line_end - line_start)); !consumed)
      return std::unexpected(std::move(consumed.error()));
    if (newline == std::string::npos)
      break;
    line_start = newline + 1;
  }

  if (entries.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session import file has no entries");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }

  auto validation = ava::session::validate_session_replay(entries);
  if (!validation.ok())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session import failed validation");
    error.with_context("path", path.string());
    error.with_context("errors", std::to_string(validation.error_count));
    if (!validation.issues.empty())
      error.with_context("first_issue", validation.issues.front().message);
    return std::unexpected(std::move(error));
  }
  auto const incomplete_output = std::ranges::find_if(validation.issues, [](ava::session::SessionReplayIssue const& issue) {
    return issue.kind == ava::session::SessionReplayIssueKind::IncompleteAssistantTurn;
  });
  if (incomplete_output != validation.issues.end())
  {
    auto error = ava::core::Error(
        ava::core::ErrorCategory::Session,
        "session import has incomplete final assistant turn; recover the source under its lease or remove the uncommitted staging records before importing");
    error.with_context("path", path.string());
    error.with_context("entry_id", incomplete_output->entry_id);
    return std::unexpected(std::move(error));
  }
  if (auto attachments = validate_jsonl_import_attachments(entries, path); !attachments)
    return std::unexpected(std::move(attachments.error()));
  return entries;
}

}  // namespace

void set_after_session_import_open_for_test(std::function<void()> hook)
{
  after_session_import_open_for_test = std::move(hook);
}

ava::core::Result<CommandResult> run_import_command(runtime::session_ts& unlocked_session, std::string_view argument)
{
  CommandResult result;
  result.handled = true;
  auto const args = split_command_arguments(argument);
  if (args.empty())
  {
    add_output(result, "usage: /import <path.jsonl> --confirm");
    return result;
  }

  bool confirm = false;
  std::optional<std::string> path_arg;
  for (auto const& arg : args)
  {
    if (arg == "--confirm")
    {
      confirm = true;
      continue;
    }
    if (!path_arg)
    {
      path_arg = arg;
      continue;
    }
    add_output(result, "usage: /import <path.jsonl> --confirm");
    return result;
  }
  if (!path_arg)
  {
    add_output(result, "usage: /import <path.jsonl> --confirm");
    return result;
  }

  auto import_path = std::filesystem::path(*path_arg);
  if (import_path.is_relative())
    import_path = runtime::session_ts::rat(unlocked_session)->current_dir() / import_path;
  import_path = import_path.lexically_normal();

  auto entries = load_import_session_entries(import_path);
  if (!entries)
  {
    add_output(result, entries.error().format());
    return result;
  }
  if (!confirm)
  {
    add_output(result, "session import is ready: " + import_path.string() + "\n  entries: " + std::to_string(entries->size()) +
                           "\n  re-run with --confirm to create a new AVA session and switch to it");
    return result;
  }

  std::filesystem::path workspace_dir;
  std::filesystem::path sessions_dir;
  runtime::OpenContext owned_options;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    workspace_dir = session_r->workspace_dir();
    sessions_dir = session_r->paths().sessions_dir;
    owned_options = session_r->replacement_open_context({});
  }
  auto imported_store = ava::session::SessionStore::create(workspace_dir, sessions_dir);
  if (!imported_store)
  {
    add_output(result, imported_store.error().format());
    return result;
  }
  auto imported_lease = ava::session::SessionLease::create_and_acquire(imported_store->session_path());
  if (!imported_lease)
  {
    add_output(result, imported_lease.error().format());
    return result;
  }
  // Preserve canonical v0 entries (whose wire form omits `version`) exactly.
  // Relabeling a legacy payload as v4 invents semantics it never carried.
  auto const& copied_entries = *entries;
  if (auto copied = imported_store->append_validated_copy(*imported_lease, copied_entries); !copied)
  {
    auto error = std::move(copied.error());
    attach_created_session_cleanup_context(*imported_store, *imported_lease, error);
    add_output(result, error.format());
    return result;
  }

  auto unlocked_opened_result = runtime::Session::open_owned(owned_options, *imported_store, *imported_lease, true);
  if (!unlocked_opened_result)
  {
    auto error = std::move(unlocked_opened_result.error());
    attach_created_session_cleanup_context(*imported_store, *imported_lease, error);
    add_output(result, error.format());
    return result;
  }
  if (auto replaced = runtime::Session::replace_with(unlocked_session, *unlocked_opened_result); !replaced)
    return std::unexpected(std::move(replaced.error()));
  auto const imported_session_id = runtime::session_ts::rat(unlocked_session)->store.session_id();
  result.session_tree_changed = true;
  add_output(result, "imported session " + imported_session_id + " from " + import_path.string() + "\n  entries: " + std::to_string(entries->size()) +
                         "\n  switched to " + imported_session_id);
  return result;
}

ava::core::Result<CommandResult> run_recover_persistence_command(runtime::session_ts& unlocked_session)
{
  CommandResult result;
  result.handled = true;
  auto run_controller = runtime::session_ts::rat(unlocked_session)->run_controller();
  if (!run_controller)
  {
    add_output(result, "session append controller is unavailable");
    return result;
  }
  if (auto recovered = run_controller->reset_persistence_failure(); !recovered)
  {
    add_output(result, recovered.error().format());
    return result;
  }
  add_output(result, "session persistence recovered; append failure latch cleared");
  return result;
}

ava::core::Result<CommandResult> run_export_command(runtime::session_ts& unlocked_session, CommandRequest const& request)
{
  CommandResult result;
  result.handled = true;
  auto parsed = parse_export_command_arguments(command_argument(request.command, "/export"));
  if (!parsed)
  {
    add_output(result, parsed.error().format());
    return result;
  }
  auto entries = load_runtime_entries(unlocked_session);
  if (!entries)
  {
    add_output(result, entries.error().format());
    return result;
  }

  auto const format = parsed->format;
  std::string content;
  if (format == ExportFormat::Html)
  {
    auto html = ava::session::format_session_html_checked(*entries);
    if (!html)
    {
      add_output(result, html.error().format());
      return result;
    }
    content = std::move(*html);
  }
  else if (format == ExportFormat::Jsonl)
  {
    auto jsonl = format_session_jsonl(*entries);
    if (!jsonl)
    {
      add_output(result, jsonl.error().format());
      return result;
    }
    content = std::move(*jsonl);
  }
  else
  {
    auto markdown = ava::session::format_session_markdown_checked(*entries);
    if (!markdown)
    {
      add_output(result, markdown.error().format());
      return result;
    }
    content = std::move(*markdown);
  }
  if (parsed->path.empty())
  {
    add_output(result, std::move(content));
    return result;
  }

  auto const target_path = resolve_export_path(unlocked_session, parsed->path);
  auto context = make_tool_context(unlocked_session, request.permission_resolver);
  context.permission_request_ids = std::make_shared<std::vector<std::string>>();
  auto linked_permission_ids = [&]() -> std::vector<std::string> {
    return context.permission_request_ids ? *context.permission_request_ids : std::vector<std::string>{};
  };

  auto const call_id = ava::core::make_id("cmd");
  auto const current_dir = runtime::session_ts::rat(unlocked_session)->current_dir();
  auto const path_text = display_path(target_path, current_dir);
  if (auto recorded = record_tool_start(unlocked_session, request.event_sink, result, call_id, "export", path_text); !recorded)
    return std::unexpected(std::move(recorded.error()));

  auto written = ava::tools::write_file(context, target_path, content);
  if (!written)
  {
    auto const text = written.error().format();
    if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "export", ava::agent::ToolTimelineStatus::Error, text, {},
                                           linked_permission_ids());
        !recorded)
      return std::unexpected(std::move(recorded.error()));
    add_output(result, text);
    return result;
  }

  auto result_json = std::string("{\"tool\":\"export\",\"ok\":true,\"format\":\"") + export_format_text(format) + "\",\"path\":\"" +
                     ava::core::json::escape(target_path.string()) + "\",\"bytes_written\":" + std::to_string(written->bytes_written);
  result_json += "}";
  if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "export", ava::agent::ToolTimelineStatus::Success,
                                         "wrote " + std::to_string(written->bytes_written) + " bytes", result_json, linked_permission_ids());
      !recorded)
    return std::unexpected(std::move(recorded.error()));
  auto output = "exported session:\n  format: " + export_format_text(format) + "\n  path: " + target_path.string() +
                "\n  bytes: " + std::to_string(written->bytes_written);
  add_output(result, std::move(output));
  return result;
}

}  // namespace ava::app

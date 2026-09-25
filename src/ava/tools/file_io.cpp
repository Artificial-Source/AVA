#include "sys.h"
#include "ava/tools/file_io.h"
#include "ava/tools/secure_workspace.h"
#include "ava/core/ids.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>
#include <unistd.h>

namespace ava::tools::detail {

namespace {

constexpr std::uintmax_t kMaxSafeReadBytes = 10 * 1024 * 1024;

std::size_t logical_line_count(std::string_view text)
{
  if (text.empty())
    return 0;
  auto const newline_count = static_cast<std::size_t>(std::ranges::count(text, '\n'));
  return text.back() == '\n' ? newline_count : newline_count + 1;
}

void trim_partial_final_line(std::string& text)
{
  auto const newline = text.find_last_of('\n');
  if (newline == std::string::npos || newline + 1 == text.size())
    return;
  text.resize(newline + 1);
}

ava::core::Result<std::filesystem::path> resolve_exact_file_path(ToolContext const& context, std::filesystem::path const& path, SecureWorkspaceResolveMode mode,
                                                                 bool require_regular_file)
{
  auto workspace = context.secure_workspace;
  if (!workspace)
  {
    auto opened = SecureWorkspace::open(context.workspace_dir);
    if (!opened)
      return std::unexpected(std::move(opened.error()));
    workspace = std::move(*opened);
  }
  auto resolved = workspace->resolve(path, mode);
  if (!resolved)
    return std::unexpected(std::move(resolved.error()));
  if (require_regular_file && resolved->exists && !resolved->regular_file)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "path is not a regular file");
    error.with_context("path", resolved->absolute.string());
    return std::unexpected(std::move(error));
  }
  if (!require_regular_file && resolved->exists && !resolved->regular_file)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "file write target is not a regular file");
    error.with_context("path", resolved->absolute.string());
    return std::unexpected(std::move(error));
  }
  return std::move(resolved->absolute);
}

TextOutput bounded_text_output(std::string_view content, ReadOptions const& options)
{
  TextOutput output;
  output.start_line = options.offset_line == 0 ? 1 : options.offset_line;
  auto const end_line_exclusive = options.max_lines == 0 || output.start_line > std::numeric_limits<std::size_t>::max() - options.max_lines
                                      ? std::numeric_limits<std::size_t>::max()
                                      : output.start_line + options.max_lines;
  std::size_t current_line = 1;
  std::size_t selected_total_bytes = 0;
  bool saw_any_byte = false;
  bool previous_was_newline = false;
  output.total_bytes = content.size();
  for (char const ch : content)
  {
    saw_any_byte = true;
    bool const in_requested_range = current_line >= output.start_line && current_line < end_line_exclusive;
    if (in_requested_range)
    {
      ++selected_total_bytes;
      output.end_line = current_line;
      if (output.content.size() < options.max_bytes)
        output.content.push_back(ch);
    }
    if (ch == '\n')
    {
      previous_was_newline = true;
      ++current_line;
    }
    else
      previous_was_newline = false;
  }
  output.total_lines = saw_any_byte ? (previous_was_newline ? current_line - 1 : current_line) : 0;
  if (output.content.size() < selected_total_bytes)
    trim_partial_final_line(output.content);
  output.output_bytes = output.content.size();
  output.output_lines = logical_line_count(output.content);
  output.byte_limited = output.output_bytes < selected_total_bytes;
  output.line_limited = options.max_lines > 0 && end_line_exclusive != std::numeric_limits<std::size_t>::max() && output.total_lines >= end_line_exclusive;
  output.truncated = output.byte_limited || output.line_limited;
  output.end_line = output.output_lines > 0 ? output.start_line + output.output_lines - 1 : 0;
  if (output.line_limited && !output.byte_limited && output.end_line > 0 && output.end_line < output.total_lines)
    output.next_offset_line = output.end_line + 1;
  return output;
}

TextOutput bounded_window_text_output(std::string_view content, ReadOptions options)
{
  auto const requested_start_line = options.offset_line == 0 ? 1 : options.offset_line;
  options.offset_line = 1;
  auto output = bounded_text_output(content, options);
  output.start_line = requested_start_line;
  output.end_line = output.output_lines > 0 ? requested_start_line + output.output_lines - 1 : 0;
  if (output.next_offset_line > 0)
    output.next_offset_line += requested_start_line - 1;
  output.totals_known = false;
  output.total_bytes = 0;
  output.total_lines = 0;
  return output;
}

ava::core::Result<std::uintmax_t> inspect_regular_read_file(std::filesystem::path const& path, std::string_view operation)
{
  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(path, status_error);
  if (status_error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect file for reading");
    error.with_context("operation", std::string(operation));
    error.with_context("path", path.string());
    error.with_context("cause", status_error.message());
    return std::unexpected(std::move(error));
  }
  if (std::filesystem::is_symlink(status))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "file reads do not follow symlinks");
    error.with_context("operation", std::string(operation));
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  if (!std::filesystem::is_regular_file(status))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "path is not a regular file");
    error.with_context("operation", std::string(operation));
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  std::error_code size_error;
  auto const size = std::filesystem::file_size(path, size_error);
  if (size_error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect file size");
    error.with_context("operation", std::string(operation));
    error.with_context("path", path.string());
    error.with_context("cause", size_error.message());
    return std::unexpected(std::move(error));
  }
  if (size > kMaxSafeReadBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "file is too large to read safely");
    error.with_context("operation", std::string(operation));
    error.with_context("path", path.string());
    error.with_context("max_bytes", std::to_string(kMaxSafeReadBytes));
    return std::unexpected(std::move(error));
  }
  return size;
}

}  // namespace

ava::core::Error io_error(std::string message, std::filesystem::path const& path, std::string cause);
std::string errno_cause(int value);

bool is_canceled(ToolContext const& context)
{
  return context.cancel_requested && context.cancel_requested();
}

ava::core::Error canceled_error(std::string_view operation, std::filesystem::path const& path)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "tool canceled", ava::core::ErrorCode::Canceled);
  error.with_context("canceled", "true");
  error.with_context("operation", std::string(operation));
  if (!path.empty())
    error.with_context("path", path.string());
  return error;
}

ava::core::VoidResult check_canceled(ToolContext const& context, std::string_view operation, std::filesystem::path const& path)
{
  if (!is_canceled(context))
    return {};
  return std::unexpected(canceled_error(operation, path));
}

bool is_canceled_error(ava::core::Error const& error)
{
  return error.code() == ava::core::ErrorCode::Canceled;
}

ava::core::Result<std::string> read_all_text_local_only(ToolContext const& context, std::filesystem::path const& path, std::string_view operation)
{
  if (auto canceled = check_canceled(context, operation, path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  if (context.secure_workspace)
  {
    auto opened = context.secure_workspace->open_regular_file(path);
    if (!opened)
      return std::unexpected(std::move(opened.error()));
    if (opened->size() > kMaxSafeReadBytes)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "file is too large for exact edit");
      error.with_context("path", opened->path().string());
      error.with_context("max_bytes", std::to_string(kMaxSafeReadBytes));
      return std::unexpected(std::move(error));
    }
    std::string content;
    content.reserve(static_cast<std::size_t>(opened->size()));
    std::array<char, 4096> buffer{};
    while (true)
    {
      if (auto canceled = check_canceled(context, operation, opened->path()); !canceled)
        return std::unexpected(std::move(canceled.error()));
      auto const count = ::read(opened->fd(), buffer.data(), buffer.size());
      if (count < 0)
      {
        if (errno == EINTR)
          continue;
        return std::unexpected(io_error("failed while reading secure workspace file", opened->path(), errno_cause(errno)));
      }
      if (count == 0)
        break;
      content.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return content;
  }

  auto inspected = inspect_regular_read_file(path, operation);
  if (!inspected)
    return std::unexpected(std::move(inspected.error()));
  constexpr std::size_t max_edit_file_bytes = static_cast<std::size_t>(kMaxSafeReadBytes);
  std::ifstream file(path, std::ios::binary);
  if (!file)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to open file for reading");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  std::string content;
  content.reserve(4096);
  std::array<char, 4096> buffer{};
  while (file)
  {
    if (auto canceled = check_canceled(context, operation, path); !canceled)
    {
      return std::unexpected(std::move(canceled.error()));
    }
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    auto const count = file.gcount();
    if (count > 0)
    {
      if (content.size() + static_cast<std::size_t>(count) > max_edit_file_bytes)
      {
        auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "file is too large for exact edit");
        error.with_context("path", path.string());
        error.with_context("max_bytes", std::to_string(max_edit_file_bytes));
        return std::unexpected(std::move(error));
      }
      content.append(buffer.data(), static_cast<std::size_t>(count));
    }
  }
  if (auto canceled = check_canceled(context, operation, path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  if (!file.eof() && file.fail())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed while reading file");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  return content;
}

ava::core::Result<std::string> read_all_text(ToolContext const& context, std::filesystem::path const& path, std::string_view operation)
{
  if (!context.exact_file_access || !context.exact_file_access->supports_read_text_file())
    return read_all_text_local_only(context, path, operation);
  if (auto canceled = check_canceled(context, operation, path); !canceled)
    return std::unexpected(std::move(canceled.error()));

  auto resolved = resolve_exact_file_path(context, path, SecureWorkspaceResolveMode::AllowMissing, true);
  if (!resolved)
    return std::unexpected(std::move(resolved.error()));
  auto content = context.exact_file_access->read_text_file(*resolved, context.cancel_requested);
  if (!content)
    return std::unexpected(std::move(content.error()));
  if (content->size() > kMaxSafeReadBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "file is too large for exact edit");
    error.with_context("path", resolved->string());
    error.with_context("max_bytes", std::to_string(kMaxSafeReadBytes));
    return std::unexpected(std::move(error));
  }
  return content;
}

ava::core::Result<TextOutput> read_head_text(ToolContext const& context, std::filesystem::path const& path, ReadOptions options)
{
  if (auto canceled = check_canceled(context, "read_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  if (context.exact_file_access && context.exact_file_access->supports_read_text_file())
  {
    auto resolved = resolve_exact_file_path(context, path, SecureWorkspaceResolveMode::AllowMissing, true);
    if (!resolved)
      return std::unexpected(std::move(resolved.error()));
    ava::tools::ExactFileReadOptions window{.line = options.offset_line == 0 ? 1 : options.offset_line, .limit = std::nullopt};
    if (options.max_lines > 0)
    {
      if (options.max_lines == std::numeric_limits<std::size_t>::max())
      {
        auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "read_file line limit overflows its sentinel window");
        error.with_context("path", resolved->string());
        return std::unexpected(std::move(error));
      }
      window.limit = options.max_lines + 1;
    }
    auto content = context.exact_file_access->read_text_file_window(*resolved, window, context.cancel_requested);
    if (!content)
      return std::unexpected(std::move(content.error()));
    if (content->size() > kMaxSafeReadBytes)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "file window is too large to read safely");
      error.with_context("path", resolved->string());
      error.with_context("max_bytes", std::to_string(kMaxSafeReadBytes));
      return std::unexpected(std::move(error));
    }
    return bounded_window_text_output(*content, options);
  }
  std::optional<SecureWorkspaceHandle> secure_file;
  std::ifstream file;
  if (context.secure_workspace)
  {
    auto opened = context.secure_workspace->open_regular_file(path);
    if (!opened)
      return std::unexpected(std::move(opened.error()));
    if (opened->size() > kMaxSafeReadBytes)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "file is too large to read safely");
      error.with_context("path", opened->path().string());
      error.with_context("max_bytes", std::to_string(kMaxSafeReadBytes));
      return std::unexpected(std::move(error));
    }
    secure_file.emplace(std::move(*opened));
  }
  else
  {
    auto inspected = inspect_regular_read_file(path, "read_file");
    if (!inspected)
      return std::unexpected(std::move(inspected.error()));
    file.open(path, std::ios::binary);
    if (!file)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to open file for reading");
      error.with_context("path", path.string());
      return std::unexpected(std::move(error));
    }
  }

  TextOutput output;
  output.start_line = options.offset_line == 0 ? 1 : options.offset_line;
  auto const end_line_exclusive = options.max_lines == 0 || output.start_line > std::numeric_limits<std::size_t>::max() - options.max_lines
                                      ? std::numeric_limits<std::size_t>::max()
                                      : output.start_line + options.max_lines;
  std::size_t current_line = 1;
  std::size_t selected_total_bytes = 0;
  bool saw_any_byte = false;
  bool previous_was_newline = false;
  std::array<char, 4096> buffer{};
  while (true)
  {
    if (auto canceled = check_canceled(context, "read_file", path); !canceled)
    {
      return std::unexpected(std::move(canceled.error()));
    }
    std::size_t count = 0;
    if (secure_file)
    {
      auto const read_count = ::read(secure_file->fd(), buffer.data(), buffer.size());
      if (read_count < 0)
      {
        if (errno == EINTR)
          continue;
        return std::unexpected(io_error("failed while reading secure workspace file", secure_file->path(), errno_cause(errno)));
      }
      count = static_cast<std::size_t>(read_count);
    }
    else
    {
      file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      count = static_cast<std::size_t>(file.gcount());
    }
    if (count == 0)
      break;
    output.total_bytes += count;
    for (std::size_t index = 0; index < count; ++index)
    {
      char const ch = buffer[index];
      saw_any_byte = true;
      bool const in_requested_range = current_line >= output.start_line && current_line < end_line_exclusive;
      if (in_requested_range)
      {
        ++selected_total_bytes;
        output.end_line = current_line;
        if (output.content.size() < options.max_bytes)
        {
          output.content.push_back(ch);
        }
      }
      if (ch == '\n')
      {
        previous_was_newline = true;
        ++current_line;
      }
      else
      {
        previous_was_newline = false;
      }
    }
  }
  if (auto canceled = check_canceled(context, "read_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  if (!secure_file && !file.eof() && file.fail())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed while reading file");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  output.total_lines = saw_any_byte ? (previous_was_newline ? current_line - 1 : current_line) : 0;
  if (output.content.size() < selected_total_bytes)
  {
    trim_partial_final_line(output.content);
  }
  output.output_bytes = output.content.size();
  output.output_lines = logical_line_count(output.content);
  output.byte_limited = output.output_bytes < selected_total_bytes;
  output.line_limited = options.max_lines > 0 && end_line_exclusive != std::numeric_limits<std::size_t>::max() && output.total_lines >= end_line_exclusive;
  output.truncated = output.byte_limited || output.line_limited;
  output.end_line = output.output_lines > 0 ? output.start_line + output.output_lines - 1 : 0;
  if (output.line_limited && !output.byte_limited && output.end_line > 0 && output.end_line < output.total_lines)
  {
    output.next_offset_line = output.end_line + 1;
  }
  return output;
}

std::filesystem::path write_parent_path(std::filesystem::path const& path)
{
  auto const parent = path.parent_path();
  if (!parent.empty())
    return parent;
  return ".";
}

std::filesystem::path unique_write_temp_path(std::filesystem::path const& target)
{
  auto const parent = write_parent_path(target);
  auto const filename = target.filename().empty() ? std::string("file") : target.filename().string();
  auto const stem = "." + filename + ".ava-write-";
  for (int attempt = 0; attempt < 8; ++attempt)
  {
    auto candidate = parent / (stem + ava::core::make_id("tmp") + ".tmp");
    std::error_code exists_error;
    if (!std::filesystem::exists(candidate, exists_error) && !exists_error)
      return candidate;
  }
  return parent / (stem + ava::core::make_id("tmp") + ".tmp");
}

ava::core::Error io_error(std::string message, std::filesystem::path const& path, std::string cause)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
  error.with_context("path", path.string());
  if (!cause.empty())
    error.with_context("cause", std::move(cause));
  return error;
}

ava::core::Error staged_io_error(std::string message, std::filesystem::path const& target_path, std::filesystem::path const& temp_path, std::string cause)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
  error.with_context("path", target_path.string());
  error.with_context("temp_path", temp_path.string());
  if (!cause.empty())
    error.with_context("cause", std::move(cause));
  return error;
}

std::string errno_cause(int value)
{
  if (value == 0)
    return "stream operation failed";
  return std::generic_category().message(value);
}

ava::core::Result<FileMutationResult> write_file_unlocked(ToolContext const& context, std::filesystem::path const& path, std::string_view content)
{
  if (auto canceled = check_canceled(context, "write_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  if (context.exact_file_access && context.exact_file_access->supports_write_text_file())
  {
    auto resolved = resolve_exact_file_path(context, path, SecureWorkspaceResolveMode::AllowMissing, false);
    if (!resolved)
      return std::unexpected(std::move(resolved.error()));
    if (auto written = context.exact_file_access->write_text_file(*resolved, content, context.cancel_requested); !written)
      return std::unexpected(std::move(written.error()));
    return FileMutationResult{
        .path = std::move(*resolved), .bytes_written = content.size(), .diff = {}, .diff_truncated = false, .line_endings = {}, .had_utf8_bom = false};
  }
  if (context.secure_workspace)
  {
    auto written = context.secure_workspace->write_file(path, content, context.cancel_requested);
    if (!written)
      return std::unexpected(std::move(written.error()));
    return FileMutationResult{.path = std::move(written->path),
                              .bytes_written = written->bytes_written,
                              .diff = {},
                              .diff_truncated = false,
                              .line_endings = {},
                              .had_utf8_bom = false};
  }
  auto const parent_path = path.parent_path();
  if (!parent_path.empty())
  {
    std::error_code mkdir_error;
    std::filesystem::create_directories(parent_path, mkdir_error);
    if (mkdir_error)
    {
      return std::unexpected(io_error("failed to create parent directory", parent_path, mkdir_error.message()));
    }
  }
  if (auto canceled = check_canceled(context, "write_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }

  std::error_code status_error;
  auto const existing_status = std::filesystem::status(path, status_error);
  bool const preserve_permissions = !status_error && std::filesystem::exists(existing_status);
  auto const existing_permissions = existing_status.permissions();

  auto const temp_path = unique_write_temp_path(path);
  if (auto canceled = check_canceled(context, "write_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  errno = 0;
  std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
  if (!file)
  {
    return std::unexpected(staged_io_error("failed to open temporary file for writing", path, temp_path, errno_cause(errno)));
  }
  errno = 0;
  constexpr std::size_t kWriteChunkBytes = 4096;
  std::size_t offset = 0;
  while (offset < content.size())
  {
    if (auto canceled = check_canceled(context, "write_file", path); !canceled)
    {
      file.close();
      remove_staged_file_best_effort(temp_path);
      return std::unexpected(std::move(canceled.error()));
    }
    auto const count = std::min(kWriteChunkBytes, content.size() - offset);
    file.write(content.data() + offset, static_cast<std::streamsize>(count));
    if (!file)
    {
      file.close();
      remove_staged_file_best_effort(temp_path);
      return std::unexpected(staged_io_error("failed to write temporary file", path, temp_path, errno_cause(errno)));
    }
    offset += count;
  }
  if (auto canceled = check_canceled(context, "write_file", path); !canceled)
  {
    file.close();
    remove_staged_file_best_effort(temp_path);
    return std::unexpected(std::move(canceled.error()));
  }
  errno = 0;
  file.close();
  if (!file)
  {
    remove_staged_file_best_effort(temp_path);
    return std::unexpected(staged_io_error("failed to close temporary file after writing", path, temp_path, errno_cause(errno)));
  }

  if (auto canceled = check_canceled(context, "write_file", path); !canceled)
  {
    remove_staged_file_best_effort(temp_path);
    return std::unexpected(std::move(canceled.error()));
  }
  if (preserve_permissions)
  {
    std::error_code permissions_error;
    std::filesystem::permissions(temp_path, existing_permissions, std::filesystem::perm_options::replace, permissions_error);
    if (permissions_error)
    {
      remove_staged_file_best_effort(temp_path);
      return std::unexpected(staged_io_error("failed to apply target permissions to temporary file", path, temp_path, permissions_error.message()));
    }
  }
  else
  {
    std::error_code permissions_error;
    std::filesystem::permissions(temp_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace,
                                 permissions_error);
    if (permissions_error)
    {
      remove_staged_file_best_effort(temp_path);
      return std::unexpected(staged_io_error("failed to apply new file permissions to temporary file", path, temp_path, permissions_error.message()));
    }
  }

  if (auto canceled = check_canceled(context, "write_file", path); !canceled)
  {
    remove_staged_file_best_effort(temp_path);
    return std::unexpected(std::move(canceled.error()));
  }
  if (auto committed = replace_file_with_staged_file(temp_path, path); !committed)
  {
    remove_staged_file_best_effort(temp_path);
    return std::unexpected(std::move(committed.error()));
  }

  FileMutationResult result;
  result.path = path;
  result.bytes_written = content.size();
  return result;
}

}  // namespace ava::tools::detail

namespace ava::tools {

ava::core::VoidResult replace_file_with_staged_file(std::filesystem::path const& staged_path, std::filesystem::path const& target_path)
{
  std::error_code rename_error;
  std::filesystem::rename(staged_path, target_path, rename_error);
  if (!rename_error)
    return {};

  auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to commit staged file write");
  error.with_context("path", target_path.string());
  error.with_context("temp_path", staged_path.string());
  error.with_context("cause", rename_error.message());
  return std::unexpected(std::move(error));
}

void remove_staged_file_best_effort(std::filesystem::path const& staged_path)
{
  std::error_code remove_error;
  std::filesystem::remove(staged_path, remove_error);
}

}  // namespace ava::tools

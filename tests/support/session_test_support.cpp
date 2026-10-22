#include "sys.h"
#include "tests/support/session_test_support.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>

namespace ava::tests {
bool session_replay_has_issue(session::SessionReplayValidation const& validation, session::SessionReplayIssueKind kind)
{
  return std::ranges::any_of(validation.issues, [kind](session::SessionReplayIssue const& issue) { return issue.kind == kind; });
}

std::string session_test_tiny_png_bytes()
{
  std::string bytes;
  bytes.push_back(static_cast<char>(0x89));
  bytes += "PNG\r\n";
  bytes.push_back(static_cast<char>(0x1A));
  bytes += "\nava-image";
  return bytes;
}

void write_test_file(std::filesystem::path const& path, std::string_view bytes, mode_t mode)
{
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  std::ofstream file;
  file.exceptions(std::ios::failbit | std::ios::badbit);
  try
  {
    file.open(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
  }
  catch (std::ios_base::failure const& error)
  {
    throw std::filesystem::filesystem_error("write test file", path, error.code());
  }
  if (::chmod(path.c_str(), mode) != 0)
    throw std::filesystem::filesystem_error("chmod test file", path, std::error_code(errno, std::generic_category()));
}

bool write_executable_for_test(std::filesystem::path const& path, std::string_view bytes)
{
  write_test_file(path, bytes, 0700);
  return true;
}

void write_session_test_binary_file(std::filesystem::path const& path, std::string_view bytes)
{
  write_test_file(path, bytes);
}

std::string read_session_test_binary_file(std::filesystem::path const& path)
{
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
}  // namespace ava::tests

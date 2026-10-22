#include "sys.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/agent/mode.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/search_tools.h"
#include "ava/tools/secure_workspace.h"
#include "ava/permissions/permission.h"
#include "ava/core/error.h"
#include "ava/core/path.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string read_text_file_for_test(std::filesystem::path const& path)
{
  std::ifstream file(path, std::ios::binary);
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

class CountingDifferentContentExactFileAccess final : public ava::tools::ExactFileAccess
{
 public:
  explicit CountingDifferentContentExactFileAccess(std::string content) : content_(std::move(content)) { }

  [[nodiscard]] ava::core::Result<std::string> read_text_file(std::filesystem::path const&, ava::tools::ToolIoCancelCallback) const override
  {
    ++read_calls;
    return content_;
  }

  [[nodiscard]] ava::core::VoidResult write_text_file(std::filesystem::path const&, std::string_view, ava::tools::ToolIoCancelCallback) const override
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "unexpected exact-file write"));
  }

  mutable int read_calls = 0;

 private:
  std::string content_;
};

// A delayed watchdog opens each FIFO for writing so a blocking regression fails
// without hanging the suite. Report whether a writer actually unblocked a read;
// ordinary scheduler delays are not a test oracle.
template <typename Operation>
auto run_with_fifo_watchdog(std::vector<std::filesystem::path> const& fifos, Operation&& operation)
{
  std::mutex mutex;
  std::condition_variable changed;
  bool finished = false;
  bool unblocked = false;
  ava::core::JoinThread unblocker = ava::core::JoinThread::create("fifo_unblocker", [&] {
    std::unique_lock lock(mutex);
    if (changed.wait_until(lock, ava::tests::now_plus_seconds(5), [&] { return finished; }))
      return;
    while (!changed.wait_for(lock, std::chrono::milliseconds(50), [&] { return finished; }))
    {
      for (auto const& fifo : fifos)
      {
        int const writer = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (writer >= 0)
        {
          unblocked = true;
          static_cast<void>(::close(writer));
        }
      }
    }
  });
  auto result = [&] {
    struct NotifyCompletion
    {
      std::mutex& mutex;
      std::condition_variable& changed;
      bool& finished;
      ~NotifyCompletion()
      {
        {
          std::lock_guard lock(mutex);
          finished = true;
        }
        changed.notify_all();
      }
    } completion{mutex, changed, finished};
    return std::forward<Operation>(operation)();
  }();
  unblocker.join();
  return std::pair{std::move(result), unblocked};
}

void test_search_tools()
{
  auto const root = create_empty_root("test_search_tools");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  expect(::chmod(workspace.c_str(), 0700) == 0, "search workspace is owner-only");
  ava::tools::ToolContext const context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build};
  expect(ava::tools::write_file(context, workspace / "src" / "main.cpp", "int main() { return 0; }\n").has_value(), "search setup writes source");
  expect(ava::tools::write_file(context, workspace / "root.cpp", "int root() { return 0; }\n").has_value(), "search setup writes root source");
  expect(ava::tools::write_file(context, workspace / "docs" / "plan.md", "hello ava\nhello again\n").has_value(), "search setup writes docs");
  expect(ava::tools::write_file(context, workspace / "build" / "ignored.txt", "hello hidden\n").has_value(), "search setup writes ignored file");
  {
    std::ofstream secret_file(workspace / ".env", std::ios::binary | std::ios::trunc);
    secret_file << "hello secret\n";
  }
  std::filesystem::create_directories(workspace / ".ssh");
  {
    std::ofstream key_file(workspace / ".ssh" / "id_rsa", std::ios::binary | std::ios::trunc);
    key_file << "hello key\n";
  }

  auto glob = ava::tools::glob_files(context, "**/*.cpp");
  expect(glob.has_value(), "glob_files succeeds");
  if (glob)
  {
    expect(glob->paths.size() == 2, "glob_files returns nested and root source files");
    expect(glob->paths.size() < 2 || glob->paths[0].generic_string() < glob->paths[1].generic_string(),
           "glob_files sorts results deterministically by generic path string");
  }

  auto bracket_glob = ava::tools::glob_files(context, "*.[ch]");
  expect(!bracket_glob && bracket_glob.error().category() == ava::core::ErrorCategory::InvalidArgument &&
             bracket_glob.error().message().find("bracket") != std::string::npos,
         "glob_files rejects unsupported bracket character classes instead of silently mis-matching them");

  auto oversized_glob = ava::tools::glob_files(context, std::string(513, '*'));
  expect(!oversized_glob && oversized_glob.error().category() == ava::core::ErrorCategory::InvalidArgument &&
             oversized_glob.error().message().find("maximum length") != std::string::npos,
         "glob_files rejects oversized patterns before traversal");

  auto result_capped = ava::tools::glob_files(context, "**/*.cpp", ava::tools::GlobOptions{.max_results = 1});
  expect(result_capped && result_capped->paths.size() == 1 && result_capped->total_matches == 2 && result_capped->truncated,
         "glob_files reports result-count truncation while counting all matches");

  auto const spill_dir = root / "session" / "spill";
  ava::tools::ToolContext const spilling_context{
      .workspace_dir = workspace, .spill_dir = spill_dir, .mode = ava::agent::Mode::Build, .current_tool_name = "glob", .current_call_id = "call/glob"};
  auto spilling_glob = ava::tools::glob_files(spilling_context, "**/*.cpp", ava::tools::GlobOptions{.max_results = 1});
  expect(spilling_glob && spilling_glob->truncated && !spilling_glob->spill_path.empty() && spilling_glob->spill_path.parent_path() == spill_dir &&
             std::filesystem::exists(spilling_glob->spill_path),
         "glob_files writes truncated results to the configured spill directory");
  if (spilling_glob && !spilling_glob->spill_path.empty())
  {
    auto const spill_text = read_text_file_for_test(spilling_glob->spill_path);
    expect(spill_text.find("main.cpp") != std::string::npos && spill_text.find("root.cpp") != std::string::npos,
           "glob spill file records one path per line for all matched paths before the result cap");
    expect(spilling_glob->spill_path.filename().string().find('/') == std::string::npos, "glob spill filename contains no user-controlled path separators");
  }

  auto capped = ava::tools::glob_files(context, "**/*", ava::tools::GlobOptions{.max_results = 2000, .max_visited = 1});
  expect(capped && capped->truncated, "glob_files reports traversal cap truncation");

  ava::tools::ToolContext const canceled_search_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .cancel_requested = [] { return true; },
  };
  auto canceled_glob = ava::tools::glob_files(canceled_search_context, "**/*");
  expect(!canceled_glob && canceled_glob.error().message() == "tool canceled", "glob_files observes cancellation before traversal");
  auto canceled_grep = ava::tools::grep_files(canceled_search_context, "hello", "**/*");
  expect(!canceled_grep && canceled_grep.error().message() == "tool canceled", "grep_files observes cancellation before search");

  auto grep = ava::tools::grep_files(context, "hello", "**/*.md");
  expect(grep.has_value(), "grep_files succeeds");
  if (grep)
  {
    expect(grep->matches.size() == 2, "grep_files returns matching markdown lines");
    expect(grep->matches[0].line_number == 1, "grep_files records line numbers");
  }
  auto const readable_link = workspace / "docs" / "plan-link.md";
  std::error_code readable_link_error;
  std::filesystem::create_symlink(workspace / "docs" / "plan.md", readable_link, readable_link_error);
  if (!readable_link_error)
  {
    auto linked_read = ava::tools::read_file(context, readable_link);
    expect(!linked_read && linked_read.error().message().find("symlink") != std::string::npos,
           "read_file rejects readable symlinks before opening file contents");
    auto linked_glob = ava::tools::glob_files(context, "**/*.md");
    expect(linked_glob && std::ranges::none_of(linked_glob->paths, [&readable_link](auto const& path) { return path == readable_link; }),
           "glob_files skips symlinked file matches by default to mirror read_file safety");
    auto linked_grep = ava::tools::grep_files(context, "hello", "**/*.md");
    expect(linked_grep && std::ranges::none_of(linked_grep->matches, [&readable_link](auto const& match) { return match.path == readable_link; }),
           "grep_files does not read symlinked files by default");
    auto explicit_link_glob = ava::tools::glob_files(context, "**/*.md", ava::tools::GlobOptions{.skip_symlinks = false});
    expect(explicit_link_glob && std::ranges::any_of(explicit_link_glob->paths, [&readable_link](auto const& path) { return path == readable_link; }),
           "glob_files only exposes symlinked matches when local code explicitly opts in");
  }
  auto ci_options = ava::tools::GrepOptions{};
  ci_options.case_insensitive = true;
  auto case_insensitive = ava::tools::grep_files(context, "HELLO", "**/*.md", ci_options);
  expect(case_insensitive && case_insensitive->matches.size() == 2, "grep_files supports explicit case-insensitive matching");
  auto regex_options = ava::tools::GrepOptions{};
  regex_options.literal = false;
  auto regex = ava::tools::grep_files(context, "hello (ava|again)", "**/*.md", regex_options);
  expect(regex && regex->matches.size() == 2, "grep_files supports provider-selected regex matching");
  auto bad_regex = ava::tools::grep_files(context, "(", "**/*.md", regex_options);
  expect(!bad_regex && bad_regex.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "grep_files reports invalid regex patterns as argument errors");

  auto listed = ava::tools::list_directory(context, workspace, ava::tools::ListDirectoryOptions{.max_entries = 20});
  auto const has_list_entry = [](ava::tools::ListDirectoryResult const& result, std::string const& name, bool directory) {
    return std::ranges::any_of(result.entries,
                               [&name, directory](ava::tools::DirectoryEntry const& entry) { return entry.name == name && entry.directory == directory; });
  };
  expect(listed && has_list_entry(*listed, "docs", true) && has_list_entry(*listed, "root.cpp", false) &&
             std::ranges::none_of(listed->entries, [](ava::tools::DirectoryEntry const& entry) { return entry.name == ".env" || entry.name == ".ssh"; }),
         "list_directory returns readable entries and omits paths denied by read policy");
  auto listed_capped = ava::tools::list_directory(context, workspace, ava::tools::ListDirectoryOptions{.max_entries = 1});
  expect(listed_capped && listed_capped->entries.size() == 1 && listed_capped->truncated && listed_capped->total_entries > listed_capped->entries.size(),
         "list_directory reports entry-count truncation");

  ava::tools::ToolContext const spilling_grep_context{
      .workspace_dir = workspace, .spill_dir = spill_dir, .mode = ava::agent::Mode::Build, .current_tool_name = "grep", .current_call_id = "call:grep"};
  auto spilling_grep = ava::tools::grep_files(spilling_grep_context, "hello", "**/*.md", ava::tools::GrepOptions{.max_matches = 1});
  expect(spilling_grep && spilling_grep->truncated && spilling_grep->matches.size() == 1 && !spilling_grep->spill_path.empty() &&
             spilling_grep->spill_path.parent_path() == spill_dir,
         "grep_files writes truncated matches to the configured spill directory");
  if (spilling_grep && !spilling_grep->spill_path.empty())
  {
    auto const spill_text = read_text_file_for_test(spilling_grep->spill_path);
    expect(spill_text.find("plan.md:1:hello ava") != std::string::npos && spill_text.find("plan.md:2:hello again") != std::string::npos,
           "grep spill file records path, line, and content for all matched lines before the result cap");
  }

  auto punctuation = ava::tools::grep_files(context, "main()", "**/*.cpp");
  expect(punctuation && !punctuation->matches.empty(), "grep_files literal search accepts punctuation");

  auto truncated = ava::tools::grep_files(context, "int", "**/*.cpp", ava::tools::GrepOptions{.max_line_length = 5});
  expect(truncated && !truncated->matches.empty() && truncated->matches[0].line_truncated, "grep_files reports line truncation metadata");

  auto ignored = ava::tools::grep_files(context, "hidden", "**/*");
  expect(ignored && ignored->matches.empty(), "grep_files skips generated folders");
  auto no_ignore_generated = ava::tools::grep_files(context, "hidden", "**/*", ava::tools::GrepOptions{.no_ignore = true});
  expect(no_ignore_generated &&
             std::ranges::any_of(no_ignore_generated->matches,
                                 [&workspace](ava::tools::GrepMatch const& match) { return match.path == workspace / "build" / "ignored.txt"; }),
         "grep_files no_ignore includes hardcoded generated-directory fallback matches");

  auto glob_secrets = ava::tools::glob_files(context, "**/*");
  expect(glob_secrets && std::ranges::none_of(glob_secrets->paths, [](std::filesystem::path const& path) { return path.filename() == "id_rsa"; }),
         "glob_files skips files denied by read policy");
  auto no_ignore_glob_secrets = ava::tools::glob_files(context, "**/*", ava::tools::GlobOptions{.no_ignore = true});
  expect(no_ignore_glob_secrets &&
             std::ranges::none_of(no_ignore_glob_secrets->paths,
                                  [](std::filesystem::path const& path) { return path.filename() == ".env" || path.filename() == "id_rsa"; }),
         "glob_files no_ignore still skips files denied by read policy");

  auto secret = ava::tools::grep_files(context, "secret", "**/*");
  expect(secret && secret->matches.empty(), "grep_files skips files denied by read policy");
  auto no_ignore_secret = ava::tools::grep_files(context, "secret", "**/*", ava::tools::GrepOptions{.no_ignore = true});
  expect(no_ignore_secret && no_ignore_secret->matches.empty(), "grep_files no_ignore still skips files denied by read policy");

  auto const outside_search_path = root / "outside-search.txt";
  {
    std::ofstream outside_file(outside_search_path, std::ios::binary | std::ios::trunc);
    outside_file << "outside hello\n";
  }
  std::error_code symlink_error;
  auto const outside_search_link = workspace / "outside-link.txt";
  std::filesystem::create_symlink(outside_search_path, outside_search_link, symlink_error);
  if (!symlink_error)
  {
    int default_search_prompts = 0;
    ava::tools::ToolContext const default_search_context{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .permission_resolver =
            [&default_search_prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          ++default_search_prompts;
          return ava::permissions::PermissionResolution::Allow;
        }};
    auto default_symlink_glob = ava::tools::glob_files(default_search_context, "**/*");
    expect(default_symlink_glob &&
               std::ranges::none_of(default_symlink_glob->paths, [&outside_search_link](auto const& path) { return path == outside_search_link; }) &&
               default_search_prompts == 0,
           "glob_files skips outside symlink matches by default before read permission prompts");
    auto default_symlink_grep = ava::tools::grep_files(default_search_context, "outside", "**/*");
    expect(default_symlink_grep && default_symlink_grep->matches.empty() && default_search_prompts == 0,
           "grep_files skips outside symlink contents by default before read permission prompts");

    int search_prompts = 0;
    std::vector<ava::tools::PermissionAuditEvent> search_audits;
    ava::tools::ToolContext const resolving_search_context{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .permission_resolver = [&search_prompts, &outside_search_link](
                                   ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          ++search_prompts;
          expect(prompt.operation == ava::permissions::Operation::ReadFile, "search resolver receives read operation for symlink escapes");
          expect(prompt.target_path == outside_search_link, "search resolver receives the matching symlink path");
          return ava::permissions::PermissionResolution::Allow;
        },
        .permission_audit_sink = [&search_audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
          search_audits.push_back(event);
          return {};
        }};
    auto resolved_glob = ava::tools::glob_files(resolving_search_context, "**/*", ava::tools::GlobOptions{.skip_symlinks = false});
    bool const resolved_includes_link =
        resolved_glob && std::ranges::any_of(resolved_glob->paths, [&outside_search_link](auto const& path) { return path == outside_search_link; });
    expect(resolved_includes_link && search_prompts == 1, "glob_files resolves ask decisions only for explicitly opted-in symlink matches");
    expect(search_audits.size() == 3 && search_audits[0].operation == ava::permissions::Operation::SearchFiles &&
               search_audits[0].action == ava::permissions::PermissionAction::Allow && search_audits[1].operation == ava::permissions::Operation::ReadFile &&
               search_audits[1].action == ava::permissions::PermissionAction::Ask && search_audits[2].resolution == "allow" &&
               search_audits[2].resolution_source == "resolver",
           "glob_files audits the search root and ask resolver outcome without per-file allow audits");

    int denied_search_prompts = 0;
    ava::tools::ToolContext const denying_search_context{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .permission_resolver =
            [&denied_search_prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          ++denied_search_prompts;
          return ava::permissions::PermissionResolution::Deny;
        }};
    auto denied_glob = ava::tools::glob_files(denying_search_context, "**/*", ava::tools::GlobOptions{.skip_symlinks = false});
    bool const denied_excludes_link =
        denied_glob && std::ranges::none_of(denied_glob->paths, [&outside_search_link](auto const& path) { return path == outside_search_link; });
    expect(denied_excludes_link && denied_search_prompts == 1, "glob_files keeps resolver-denied ask matches excluded");

    int failing_search_prompts = 0;
    ava::tools::ToolContext const failing_search_context{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .permission_resolver =
            [&failing_search_prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          ++failing_search_prompts;
          return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "search resolver failed"));
        }};
    auto failing_glob = ava::tools::glob_files(failing_search_context, "**/*", ava::tools::GlobOptions{.skip_symlinks = false});
    bool const failing_skips_link =
        failing_glob && std::ranges::none_of(failing_glob->paths, [&outside_search_link](auto const& path) { return path == outside_search_link; });
    bool const failing_keeps_readable_match =
        failing_glob && std::ranges::any_of(failing_glob->paths, [&workspace](auto const& path) { return path == workspace / "docs" / "plan.md"; });
    expect(failing_skips_link && failing_keeps_readable_match && failing_search_prompts == 1,
           "glob_files skips ask matches with resolver errors while continuing search");
  }

  {
    std::ofstream binary_file(workspace / "binary.bin", std::ios::binary | std::ios::trunc);
    binary_file << std::string("binary", 6) << '\0' << " marker\nhello from binary\n";
  }
  auto binary = ava::tools::grep_files(context, "hello", "**/*.bin");
  expect(binary && binary->matches.empty() && binary->total_matches == 0, "grep_files skips an entire file after detecting binary content");
  auto no_ignore_binary = ava::tools::grep_files(context, "hello", "**/*.bin", ava::tools::GrepOptions{.no_ignore = true});
  expect(no_ignore_binary && no_ignore_binary->matches.empty() && no_ignore_binary->total_matches == 0, "grep_files no_ignore still skips binary content");

  {
    std::ofstream binary_file(workspace / "overlong-binary.bin", std::ios::binary | std::ios::trunc);
    binary_file << std::string(32, 'x') << '\0' << " hello after nul\n";
  }
  auto overlong_binary = ava::tools::grep_files(context, "hello", "**/overlong-binary.bin", ava::tools::GrepOptions{.max_line_length = 5});
  expect(overlong_binary && overlong_binary->matches.empty() && overlong_binary->total_matches == 0,
         "grep_files treats NUL after an overlong truncation point as binary content");

  {
    std::ofstream binary_file(workspace / "early-match-binary.bin", std::ios::binary | std::ios::trunc);
    binary_file << "hello before binary marker\n" << '\0' << "binary tail\n";
  }
  ava::tools::ToolContext const binary_spill_context{
      .workspace_dir = workspace, .spill_dir = spill_dir, .mode = ava::agent::Mode::Build, .current_tool_name = "grep", .current_call_id = "call-binary-spill"};
  auto binary_spill_grep = ava::tools::grep_files(binary_spill_context, "hello", "**/*", ava::tools::GrepOptions{.max_matches = 1});
  expect(binary_spill_grep && binary_spill_grep->truncated && !binary_spill_grep->spill_path.empty(),
         "grep_files writes a spill file for truncated non-binary matches");
  if (binary_spill_grep && !binary_spill_grep->spill_path.empty())
  {
    auto const spill_text = read_text_file_for_test(binary_spill_grep->spill_path);
    expect(spill_text.find("hello before binary marker") == std::string::npos, "grep spill files exclude matches from files later classified as binary");
  }
}

void test_search_gitignore_rules()
{
  auto const root = create_empty_root("search-ignore");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  expect(::chmod(workspace.c_str(), 0700) == 0, "ignore-rule workspace is owner-only");
  ava::tools::ToolContext const context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build};

  expect(ava::tools::write_file(context, workspace / ".gitignore",
                                "*.log\n"
                                "!keep.log\n"
                                "cache/\n"
                                "aaa_ignored/\n"
                                "logs/**/*.tmp\n"
                                "private/   \n"
                                "customer\\ data/\n"
                                "\\#literal\n"
                                "\\!literal\n"
                                "wildcard_private/*\n"
                                "!wildcard_private/\\*.txt\n")
             .has_value(),
         "ignore setup writes root .gitignore");
  expect(ava::tools::write_file(context, workspace / "app.log", "hidden root log\n").has_value(), "ignore setup writes root ignored file");
  expect(ava::tools::write_file(context, workspace / "keep.log", "visible negated log\n").has_value(), "ignore setup writes negated file");
  expect(ava::tools::write_file(context, workspace / "cache" / "data.txt", "hidden cache\n").has_value(), "ignore setup writes directory-only ignored file");
  expect(ava::tools::write_file(context, workspace / "logs" / "deep" / "trace.tmp", "hidden double star\n").has_value(),
         "ignore setup writes double-star ignored file");
  expect(ava::tools::write_file(context, workspace / "private" / "data.txt", "hidden trailing spaces\n").has_value(),
         "ignore setup writes file ignored by trailing-space rule");
  expect(ava::tools::write_file(context, workspace / "customer data" / "data.txt", "hidden escaped space\n").has_value(),
         "ignore setup writes file ignored by escaped-space rule");
  expect(ava::tools::write_file(context, workspace / "#literal", "hidden escaped comment\n").has_value(),
         "ignore setup writes file ignored by escaped comment rule");
  expect(ava::tools::write_file(context, workspace / "!literal", "hidden escaped negation\n").has_value(),
         "ignore setup writes file ignored by escaped negation rule");
  expect(ava::tools::write_file(context, workspace / "wildcard_private" / "customer.txt", "hidden wildcard\n").has_value(),
         "ignore setup writes file hidden by wildcard directory rule");
  expect(ava::tools::write_file(context, workspace / "wildcard_private" / "*.txt", "visible literal star\n").has_value(),
         "ignore setup writes file re-included by escaped wildcard negation");
  expect(ava::tools::write_file(context, workspace / "src" / ".gitignore", "/local.txt\n").has_value(), "ignore setup writes nested .gitignore");
  expect(ava::tools::write_file(context, workspace / "src" / "local.txt", "hidden nested local\n").has_value(), "ignore setup writes nested ignored file");
  expect(ava::tools::write_file(context, workspace / "src" / "nested" / "local.txt", "visible nested child\n").has_value(),
         "ignore setup writes file outside nested anchored rule");
  for (int index = 0; index < 50; ++index)
  {
    expect(ava::tools::write_file(context, workspace / "aaa_ignored" / ("ignored" + std::to_string(index) + ".txt"), "ignored traversal budget\n").has_value(),
           "ignore setup writes ignored directory entry");
  }
  expect(ava::tools::write_file(context, workspace / "zzz_late.txt", "visible late file\n").has_value(), "ignore setup writes a late visible file");

  auto const has_path = [](ava::tools::GlobResult const& result, std::filesystem::path const& path) {
    return std::ranges::any_of(result.paths, [&path](auto const& candidate) { return candidate == path; });
  };

  auto default_glob = ava::tools::glob_files(context, "**/*");
  expect(default_glob.has_value(), "glob_files succeeds with gitignore matcher");
  if (default_glob)
  {
    expect(!has_path(*default_glob, workspace / "app.log"), "root .gitignore ignores wildcard matches");
    expect(has_path(*default_glob, workspace / "keep.log"), "root .gitignore negation re-includes later matches");
    expect(!has_path(*default_glob, workspace / "cache" / "data.txt"), "directory-only .gitignore rules ignore descendants");
    expect(!has_path(*default_glob, workspace / "aaa_ignored" / "ignored0.txt"), "directory-only .gitignore rules prune ignored directory descendants");
    expect(!has_path(*default_glob, workspace / "logs" / "deep" / "trace.tmp"), "double-star .gitignore rules ignore deep descendants");
    expect(!has_path(*default_glob, workspace / "private" / "data.txt"),
           "unescaped trailing spaces do not prevent directory-only .gitignore rules from matching");
    expect(!has_path(*default_glob, workspace / "customer data" / "data.txt"), "backslash-escaped spaces in .gitignore rules match literal spaces");
    expect(!has_path(*default_glob, workspace / "#literal"), "backslash-escaped comment markers in .gitignore rules match literal filenames");
    expect(!has_path(*default_glob, workspace / "!literal"), "backslash-escaped negation markers in .gitignore rules match literal filenames");
    expect(!has_path(*default_glob, workspace / "wildcard_private" / "customer.txt"),
           "escaped wildcard negation does not re-include ordinary wildcard matches");
    expect(has_path(*default_glob, workspace / "wildcard_private" / "*.txt"), "escaped wildcard negation re-includes only the literal wildcard filename");
    expect(!has_path(*default_glob, workspace / "src" / "local.txt"), "nested .gitignore anchored rules are relative to the nested directory");
    expect(has_path(*default_glob, workspace / "src" / "nested" / "local.txt"), "nested anchored .gitignore rules do not ignore deeper same-name files");
  }

  auto no_ignore_glob = ava::tools::glob_files(context, "**/*", ava::tools::GlobOptions{.no_ignore = true});
  expect(no_ignore_glob && has_path(*no_ignore_glob, workspace / "app.log") && has_path(*no_ignore_glob, workspace / "cache" / "data.txt") &&
             has_path(*no_ignore_glob, workspace / "aaa_ignored" / "ignored0.txt") && has_path(*no_ignore_glob, workspace / "src" / "local.txt"),
         "glob_files no_ignore opt-out returns files ignored by .gitignore");

  auto pruned_glob = ava::tools::glob_files(context, "zzz_late.txt", ava::tools::GlobOptions{.max_visited = 30});
  expect(pruned_glob && !pruned_glob->truncated && has_path(*pruned_glob, workspace / "zzz_late.txt"),
         "glob_files prunes ignored directories before they exhaust the traversal budget");

  auto const external_ignore = root / "external-ignore";
  {
    std::ofstream file(external_ignore, std::ios::binary | std::ios::trunc);
    file << "*.txt\n";
  }
  std::error_code symlink_error;
  std::filesystem::create_directories(workspace / "symlinked", symlink_error);
  symlink_error.clear();
  std::filesystem::create_symlink(external_ignore, workspace / "symlinked" / ".gitignore", symlink_error);
  expect(ava::tools::write_file(context, workspace / "symlinked" / "visible.txt", "visible symlink ignore\n").has_value(),
         "ignore setup writes file next to symlinked .gitignore");
  if (!symlink_error)
  {
    auto symlink_glob = ava::tools::glob_files(context, "symlinked/visible.txt");
    expect(symlink_glob && has_path(*symlink_glob, workspace / "symlinked" / "visible.txt"),
           "glob_files does not follow symlinked .gitignore files outside the workspace");
  }

  auto default_grep = ava::tools::grep_files(context, "hidden", "**/*");
  expect(default_grep && std::ranges::none_of(default_grep->matches,
                                              [&workspace](ava::tools::GrepMatch const& match) {
                                                return match.path == workspace / "app.log" || match.path == workspace / "cache" / "data.txt" ||
                                                       match.path == workspace / "logs" / "deep" / "trace.tmp" || match.path == workspace / "src" / "local.txt";
                                              }),
         "grep_files respects .gitignore by default");

  auto no_ignore_grep = ava::tools::grep_files(context, "hidden", "**/*", ava::tools::GrepOptions{.no_ignore = true});
  expect(no_ignore_grep &&
             std::ranges::any_of(no_ignore_grep->matches, [&workspace](ava::tools::GrepMatch const& match) { return match.path == workspace / "app.log"; }) &&
             std::ranges::any_of(no_ignore_grep->matches,
                                 [&workspace](ava::tools::GrepMatch const& match) { return match.path == workspace / "src" / "local.txt"; }),
         "grep_files no_ignore opt-out searches files ignored by .gitignore");
}

void test_secure_workspace_search_tools()
{
  auto const root = create_empty_root("secure-workspace-search");

  auto const workspace = root / "workspace";
  auto const outside = root / "outside";
  std::filesystem::create_directories(workspace / "ordinary" / "nested");
  std::filesystem::create_directories(outside);
  {
    std::ofstream file(workspace / "ordinary" / "nested" / "inside.txt", std::ios::binary | std::ios::trunc);
    file << "inside searchable text\n";
    std::ofstream secret(outside / "secret.txt", std::ios::binary | std::ios::trunc);
    secret << "outside searchable secret\n";
  }
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(outside, workspace / "linked-parent", symlink_error);

  auto secure = ava::tools::SecureWorkspace::open(ava::core::normalized_absolute_path(workspace));
  if (!ava::tests::require(secure.has_value(), "secure search workspace anchors a canonical root descriptor"))
    return;
  ava::tools::ToolContext context{.workspace_dir = ava::core::normalized_absolute_path(workspace), .mode = ava::agent::Mode::Build, .secure_workspace = *secure};
  auto list_link = ava::tools::list_directory(context, workspace / "linked-parent");
  auto glob = ava::tools::glob_files(context, "**/*.txt");
  auto grep = ava::tools::grep_files(context, "searchable", "**/*.txt");
  auto list_nested = ava::tools::list_directory(context, workspace / "ordinary" / "nested");
  expect((symlink_error || !list_link) && glob && grep && list_nested && list_nested->entries.size() == 1 &&
             std::ranges::find(glob->paths, workspace / "ordinary" / "nested" / "inside.txt") != glob->paths.end() &&
             std::ranges::none_of(glob->paths, [&workspace](auto const& path) { return path.string().find((workspace / "linked-parent").string()) == 0; }) &&
             grep->matches.size() == 1 && grep->matches.front().path == workspace / "ordinary" / "nested" / "inside.txt",
         "secure list/glob/grep reject symlinked parents while ordinary nested search remains functional");

  auto exact_access = std::make_shared<CountingDifferentContentExactFileAccess>("client-only-marker\n");
  context.exact_file_access = exact_access;
  auto local_grep = ava::tools::grep_files(context, "inside searchable text", "**/inside.txt");
  auto client_grep = ava::tools::grep_files(context, "client-only-marker", "**/inside.txt");
  expect(local_grep && local_grep->matches.size() == 1 && client_grep && client_grep->matches.empty() && exact_access->read_calls == 0,
         "secure grep reads descriptor-anchored local bytes without consulting exact-file access");
}

void test_fifos_never_block_secure_reads_or_search()
{
  auto const root = create_empty_root("fifo-reads-and-search");
  auto const workspace = ava::core::normalized_absolute_path(root / "workspace");
  std::filesystem::create_directories(workspace / "src");
  ava::tests::write_test_file(workspace / "src" / "real.cpp", "fifo needle\n");
  auto const source_fifo = workspace / "src" / "pipe.cpp";
  auto const ignore_fifo = workspace / ".gitignore";
  bool const fifos_created = ::mkfifo(source_fifo.c_str(), 0600) == 0 && ::mkfifo(ignore_fifo.c_str(), 0600) == 0;
  if (!ava::tests::require(fifos_created, "FIFO fixture creates a FIFO named like a source file and a FIFO root .gitignore"))
    return;
  auto secure = ava::tools::SecureWorkspace::open(workspace);
  if (!ava::tests::require(secure.has_value(), "FIFO fixture anchors a secure workspace"))
    return;
  std::vector<std::filesystem::path> const fifos{source_fifo, ignore_fifo};
  ava::tools::ToolContext const secure_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build, .secure_workspace = *secure};
  ava::tools::ToolContext const plain_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build};

  auto const [fifo_read, read_unblocked] = run_with_fifo_watchdog(fifos, [&] { return ava::tools::read_file(secure_context, source_fifo); });
  expect(!fifo_read && fifo_read.error().message().find("not a regular file") != std::string::npos && !read_unblocked,
         "secure read_file rejects a FIFO named like a source file without blocking in open");
  auto const [fifo_edit, edit_unblocked] = run_with_fifo_watchdog(fifos, [&] { return ava::tools::edit_file(secure_context, source_fifo, "fifo", "pipe"); });
  expect(!fifo_edit && fifo_edit.error().message().find("not a regular file") != std::string::npos && !edit_unblocked,
         "secure edit_file rejects a FIFO without blocking in open");

  auto const only_real_source = [&workspace](std::vector<std::filesystem::path> const& paths) {
    return paths.size() == 1 && paths.front() == workspace / "src" / "real.cpp";
  };
  for (auto const* context : {&secure_context, &plain_context})
  {
    auto const label = std::string(context->secure_workspace ? "secure" : "non-secure");
    auto const [glob, glob_unblocked] = run_with_fifo_watchdog(fifos, [&] { return ava::tools::glob_files(*context, "**/*.cpp"); });
    expect(glob && only_real_source(glob->paths) && !glob->truncated && glob->skipped_paths == 0 && !glob_unblocked,
           label + " glob skips a FIFO .gitignore and a FIFO source name without blocking");
    auto const [grep, grep_unblocked] = run_with_fifo_watchdog(fifos, [&] { return ava::tools::grep_files(*context, "fifo needle", "**/*.cpp"); });
    expect(grep && grep->matches.size() == 1 && grep->matches.front().path == workspace / "src" / "real.cpp" && !grep->truncated && !grep_unblocked,
           label + " grep returns regular-file matches beside FIFOs without blocking");
  }
}

void test_search_reports_unreadable_directories_as_skipped()
{
  // Permission bits do not restrict root, so the unreadable-directory fixture cannot exist for a privileged run.
  if (::geteuid() == 0)
  {
    ava::tests::request_skip("unreadable search directories", "permission-bit denial requires an unprivileged effective UID");
    return;
  }
  auto const root = create_empty_root("search-unreadable-directory");
  auto const workspace = ava::core::normalized_absolute_path(root / "workspace");
  auto const locked = workspace / "locked";
  std::filesystem::create_directories(workspace / "open");
  std::filesystem::create_directories(locked);
  {
    std::ofstream visible(workspace / "open" / "visible.txt", std::ios::binary | std::ios::trunc);
    visible << "skip needle\n";
    std::ofstream hidden(locked / "hidden.txt", std::ios::binary | std::ios::trunc);
    hidden << "skip needle\n";
  }
  auto secure = ava::tools::SecureWorkspace::open(workspace);
  bool const locked_down = ::chmod(locked.c_str(), 0) == 0;
  expect(secure.has_value() && locked_down, "unreadable-directory fixture anchors a workspace and removes all access to one subdirectory");
  if (!secure || !locked_down)
  {
    static_cast<void>(::chmod(locked.c_str(), 0700));
    return;
  }
  auto const spill = root / "spill";
  std::filesystem::create_directory(spill);
  ava::tools::ToolContext const secure_context{.workspace_dir = workspace, .spill_dir = spill, .mode = ava::agent::Mode::Build, .secure_workspace = *secure};
  ava::tools::ToolContext const plain_context{.workspace_dir = workspace, .spill_dir = spill, .mode = ava::agent::Mode::Build};
  auto secure_glob = ava::tools::glob_files(secure_context, "**/*.txt");
  auto secure_grep = ava::tools::grep_files(secure_context, "skip needle", "**/*.txt");
  auto plain_glob = ava::tools::glob_files(plain_context, "**/*.txt");
  auto plain_grep = ava::tools::grep_files(plain_context, "skip needle", "**/*.txt");
  static_cast<void>(::chmod(locked.c_str(), 0700));

  auto const visible = workspace / "open" / "visible.txt";
  auto const glob_reports_skip = [&visible](ava::core::Result<ava::tools::GlobResult> const& glob) {
    return glob && glob->paths.size() == 1 && glob->paths.front() == visible && !glob->truncated && glob->skipped_paths == 1 && glob->spill_path.empty();
  };
  auto const grep_reports_skip = [&visible](ava::core::Result<ava::tools::GrepResult> const& grep) {
    return grep && grep->matches.size() == 1 && grep->matches.front().path == visible && !grep->truncated && grep->skipped_paths == 1 &&
           grep->spill_path.empty();
  };
  expect(glob_reports_skip(secure_glob) && grep_reports_skip(secure_grep),
         "secure glob and grep report an unreadable subdirectory as skipped while returning the other matches");
  expect(glob_reports_skip(plain_glob) && grep_reports_skip(plain_grep),
         "non-secure glob and grep report an unreadable subdirectory as skipped instead of ending the walk silently");
  expect(std::filesystem::is_empty(spill), "skipped directories alone never create search spill files");
}

void test_secure_walk_counts_vanished_entry_without_stale_errno()
{
  auto const root = create_empty_root("secure-walk-vanished-entry");
  auto const workspace = ava::core::normalized_absolute_path(root / "workspace");
  std::filesystem::create_directories(workspace);
  {
    std::ofstream first(workspace / "first.txt", std::ios::binary | std::ios::trunc);
    first << "first\n";
    std::ofstream second(workspace / "second.txt", std::ios::binary | std::ios::trunc);
    second << "second\n";
  }
  auto secure = ava::tools::SecureWorkspace::open(workspace);
  if (!ava::tests::require(secure.has_value(), "vanished-entry fixture anchors a secure workspace"))
    return;
  std::vector<std::string> visited;
  bool removed = false;
  auto walked = (*secure)->visit_tree([&](ava::tools::SecureWorkspaceDirectoryEntry const& entry) -> ava::core::Result<ava::tools::SecureWorkspaceWalkAction> {
    visited.push_back(entry.name);
    if (!removed)
    {
      // readdir has already buffered both names, so the walk still reaches the removed sibling, whose fstatat fails with ENOENT. It is
      // the last regular entry, so a stale errno would surface as a failed directory read at end of stream.
      std::error_code remove_error;
      removed = std::filesystem::remove(workspace / (entry.name == "first.txt" ? "second.txt" : "first.txt"), remove_error);
    }
    return ava::tools::SecureWorkspaceWalkAction::Continue;
  });
  expect(removed && walked && walked->skipped_entries == 1 && visited.size() == 1,
         "secure walk completes and counts a sibling that vanished after readdir instead of failing on a stale errno");
}

void test_secure_grep_skips_oversized_files()
{
  auto const root = create_empty_root("secure-grep-oversized");
  auto const workspace = ava::core::normalized_absolute_path(root / "workspace");
  std::filesystem::create_directories(workspace / "data");
  auto const huge = workspace / "data" / "huge.bin";
  {
    std::ofstream small(workspace / "small.txt", std::ios::binary | std::ios::trunc);
    small << "oversize needle\n";
    std::ofstream created(huge, std::ios::binary | std::ios::trunc);
  }
  std::error_code size_error;
  std::filesystem::resize_file(huge, 11U * 1024U * 1024U, size_error);
  auto secure = ava::tools::SecureWorkspace::open(workspace);
  expect(!size_error && secure.has_value(), "oversized grep fixture creates an 11 MiB file and anchors a secure workspace");
  if (size_error || !secure)
    return;
  auto const spill = root / "spill";
  std::filesystem::create_directory(spill);
  ava::tools::ToolContext const context{.workspace_dir = workspace, .spill_dir = spill, .mode = ava::agent::Mode::Build, .secure_workspace = *secure};

  auto grep = ava::tools::grep_files(context, "oversize needle");
  expect(grep && grep->matches.size() == 1 && grep->matches.front().path == workspace / "small.txt" && !grep->truncated && grep->skipped_paths == 1 &&
             grep->spill_path.empty() && std::filesystem::is_empty(spill),
         "secure grep reports an oversized file independently from truncation without spilling complete inline matches");

  auto edited = ava::tools::edit_file(context, huge, "a", "b");
  expect(!edited && edited.error().code() == ava::core::ErrorCode::FileSizeLimit && edited.error().message().find("edit_file") != std::string::npos,
         "the read size limit is an identifiable error that names the refused operation");
}

void test_secure_listing_keeps_symlink_names()
{
  auto const root = create_empty_root("secure-list-symlink-names");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace / "real-dir");
  std::filesystem::create_directories(root / "outside");
  ava::tests::write_test_file(workspace / "real.txt", "abc");
  std::filesystem::create_directory_symlink("real-dir", workspace / "dir-link");
  std::filesystem::create_symlink("real.txt", workspace / "file-link");
  std::filesystem::create_directory_symlink(root / "outside", workspace / "escape-link");
  std::filesystem::create_symlink("missing", workspace / "dangling-link");
  auto secure = ava::tools::SecureWorkspace::open(workspace);
  if (!ava::tests::require(secure.has_value(), "symlink listing fixture opens its workspace"))
    return;
  ava::tools::ToolContext const context{.workspace_dir = workspace, .secure_workspace = *secure};
  auto listing = ava::tools::list_directory(context, workspace);
  if (!ava::tests::require(listing.has_value(), "secure directory listing succeeds with contained, escaping, and dangling links"))
    return;
  auto const has_entry = [&](std::string_view name, bool directory, std::uintmax_t size) {
    return std::ranges::any_of(listing->entries, [&](auto const& entry) { return entry.name == name && entry.directory == directory && entry.size == size; });
  };
  expect(has_entry("dir-link", true, 0) && has_entry("file-link", false, 3), "contained symlink entries report their descriptor-inspected target types");
  expect(has_entry("escape-link", false, 0) && has_entry("dangling-link", false, 0),
         "escaping and dangling symlink names remain visible without following their targets");
}

void test_search_excludes_unapproved_files()
{
  auto const root = create_empty_root("explicit-search-permissions");
  auto const workspace = ava::core::normalized_absolute_path(root / "workspace");
  std::filesystem::create_directories(workspace);
  ava::tests::write_test_file(workspace / "allowed.txt", "permission needle\n");
  ava::tests::write_test_file(workspace / "denied.txt", "permission needle\n");
  ava::tests::write_test_file(workspace / ".env", "permission needle\n");
  auto secure = ava::tools::SecureWorkspace::open(workspace);
  if (!ava::tests::require(secure.has_value(), "explicit search fixture opens a descriptor workspace"))
    return;

  ava::tools::ToolContext context{
      .workspace_dir = workspace,
      .permission_resolver = [&](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return prompt.target_path == workspace / "allowed.txt" ? ava::permissions::PermissionResolution::Allow : ava::permissions::PermissionResolution::Deny;
      },
      .auto_allow_deny_preflight = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .require_explicit_file_permissions = true,
      .secure_workspace = *secure,
      .require_descriptor_secure_workspace = true};
  context.permission_tool_name = "glob";
  auto glob = ava::tools::glob_files(context, "**/*");
  expect(glob && glob->paths == std::vector<std::filesystem::path>{workspace / "allowed.txt"},
         "glob omits files denied by the resolver or built-in secret policy");
  context.permission_tool_name = "grep";
  auto grep = ava::tools::grep_files(context, "permission needle");
  expect(grep && grep->matches.size() == 1 && grep->matches.front().path == workspace / "allowed.txt",
         "grep returns bytes only from approved files and omits denied and secret files");
  context.permission_tool_name = "list_directory";
  auto listing = ava::tools::list_directory(context, workspace);
  expect(listing && listing->entries.size() == 1 && listing->entries.front().name == "allowed.txt",
         "directory listing omits files denied by the resolver or built-in secret policy");
}

void test_gitignored_virtual_environment_is_excluded()
{
  auto const root = create_empty_root("search-ignored-venv");
  auto const workspace = ava::core::normalized_absolute_path(root / "workspace");
  std::filesystem::create_directories(workspace / ".venv" / "lib" / "package");
  std::filesystem::create_directories(workspace / "src");
  ava::tests::write_test_file(workspace / ".gitignore", ".venv/\n");
  ava::tests::write_test_file(workspace / ".venv" / "lib" / "package" / "hidden.py", "venv needle\n");
  ava::tests::write_test_file(workspace / "src" / "visible.py", "venv needle\n");
  auto secure = ava::tools::SecureWorkspace::open(workspace);
  if (!ava::tests::require(secure.has_value(), "ignored virtual environment fixture opens a descriptor workspace"))
    return;
  for (bool const secure_mode : {false, true})
  {
    ava::tools::ToolContext const context{.workspace_dir = workspace, .secure_workspace = secure_mode ? *secure : nullptr};
    auto glob = ava::tools::glob_files(context, "**/*.py");
    auto grep = ava::tools::grep_files(context, "venv needle", "**/*.py");
    expect(glob && glob->paths == std::vector<std::filesystem::path>{workspace / "src" / "visible.py"} && !glob->truncated && grep &&
               grep->matches.size() == 1 && grep->matches.front().path == workspace / "src" / "visible.py" && !grep->truncated,
           "glob and grep exclude a gitignored .venv while retaining ordinary source matches");
  }
}

void test_nested_fifo_gitignore_does_not_block_search()
{
  auto const root = create_empty_root("search-nested-fifo-ignore");
  auto const workspace = ava::core::normalized_absolute_path(root / "workspace");
  std::filesystem::create_directories(workspace / "src");
  ava::tests::write_test_file(workspace / "src" / "visible.txt", "nested fifo needle\n");
  auto const fifo = workspace / "src" / ".gitignore";
  if (::mkfifo(fifo.c_str(), 0600) != 0)
  {
    if (errno == ENOSYS || errno == EOPNOTSUPP)
      ava::tests::request_skip("nested ignore FIFO", "filesystem does not support FIFOs");
    else
      expect(false, "nested ignore FIFO setup failed: errno=" + std::to_string(errno));
    return;
  }
  auto secure = ava::tools::SecureWorkspace::open(workspace);
  if (!ava::tests::require(secure.has_value(), "nested FIFO fixture opens a descriptor workspace"))
    return;
  std::vector<std::filesystem::path> const fifos{fifo};
  for (bool const secure_mode : {false, true})
  {
    ava::tools::ToolContext const context{.workspace_dir = workspace, .secure_workspace = secure_mode ? *secure : nullptr};
    auto const [glob, glob_unblocked] = run_with_fifo_watchdog(fifos, [&] { return ava::tools::glob_files(context, "**/*.txt"); });
    auto const [grep, grep_unblocked] = run_with_fifo_watchdog(fifos, [&] { return ava::tools::grep_files(context, "nested fifo needle", "**/*.txt"); });
    expect(glob && glob->paths == std::vector<std::filesystem::path>{workspace / "src" / "visible.txt"} && !glob_unblocked && grep &&
               grep->matches.size() == 1 && grep->matches.front().path == workspace / "src" / "visible.txt" && !grep_unblocked,
           "nested FIFO .gitignore never blocks secure or non-secure searches");
  }
  ava::tools::ToolContext const context{.workspace_dir = workspace, .secure_workspace = *secure};
  auto const [read, unblocked] = run_with_fifo_watchdog(fifos, [&] { return ava::tools::read_file(context, fifo); });
  expect(!read && read.error().message().find("not a regular file") != std::string::npos && !unblocked,
         "secure read_file rejects an explicitly requested FIFO .gitignore without waiting for a writer");
}

}  // namespace

void run_tools_search_tests()
{
  test_search_tools();
  test_search_gitignore_rules();
  test_secure_workspace_search_tools();
  test_fifos_never_block_secure_reads_or_search();
  test_search_reports_unreadable_directories_as_skipped();
  test_secure_walk_counts_vanished_entry_without_stale_errno();
  test_secure_grep_skips_oversized_files();
  test_secure_listing_keeps_symlink_names();
  test_search_excludes_unapproved_files();
  test_gitignored_virtual_environment_is_excluded();
  test_nested_fifo_gitignore_does_not_block_search();
}

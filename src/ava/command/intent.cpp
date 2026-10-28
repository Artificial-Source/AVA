#include "sys.h"
#include "ava/command/intent_internal.h"
#include "ava/core/ascii.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <utility>

namespace ava::command::detail {
namespace {

constexpr std::size_t kHardMaxRequestBytes = 16 * 1024;
constexpr std::size_t kHardMaxArgumentBytes = 4 * 1024;
constexpr std::size_t kHardMaxArgvEntries = 128;
constexpr std::size_t kHardMaxPathEntries = 64;
constexpr std::size_t kHardMaxPathBytes = 16 * 1024;
constexpr std::size_t kHardMaxShebangBytes = 512;
constexpr std::size_t kHardMaxShebangDepth = 4;

bool exceeds_limit(std::size_t current, std::size_t addition, std::size_t limit)
{
  return current > limit || addition > limit - current;
}

bool is_shell_metacharacter(char ch)
{
  switch (ch)
  {
    case ';':
    case '&':
    case '|':
    case '<':
    case '>':
    case '`':
    case '$':
    case '(':
    case ')':
    case '*':
    case '?':
    case '[':
    case ']':
    case '{':
    case '}':
    case '~':
    case '#':
    case '!':
      return true;
    default:
      return false;
  }
}

bool is_assignment_word(std::string_view value)
{
  auto const equal = value.find('=');
  if (equal == std::string_view::npos || equal == 0)
    return false;
  if (std::isalpha(static_cast<unsigned char>(value.front())) == 0 && value.front() != '_')
    return false;
  return std::ranges::all_of(value.substr(1, equal - 1), [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_'; });
}

bool is_shell_command_position_word(std::string_view value)
{
  // Compatibility input is shell text, so command-position shell words must
  // never be reinterpreted as a direct executable lookup. This includes POSIX
  // special builtins, common shell builtins, and grammar/control keywords.
  static constexpr std::array<std::string_view, 78> kShellWords{
      ".",        ":",       "alias",     "bg",       "bind",    "break",   "builtin", "caller", "case",    "cd",      "chdir",    "command", "compgen",
      "complete", "compopt", "continue",  "coproc",   "declare", "dirs",    "disown",  "do",     "done",    "echo",    "elif",     "else",    "enable",
      "esac",     "eval",    "exec",      "exit",     "export",  "false",   "fc",      "fg",     "fi",      "for",     "function", "getopts", "hash",
      "help",     "history", "if",        "in",       "jobs",    "kill",    "let",     "local",  "logout",  "mapfile", "popd",     "printf",  "pushd",
      "pwd",      "read",    "readarray", "readonly", "return",  "select",  "set",     "shift",  "shopt",   "source",  "suspend",  "test",    "then",
      "time",     "times",   "trap",      "true",     "type",    "typeset", "ulimit",  "umask",  "unalias", "unset",   "until",    "wait",    "while"};
  return std::ranges::find(kShellWords, value) != kShellWords.end();
}

}  // namespace

ava::core::Error command_error(ava::core::ErrorCategory category, std::string message, std::string_view field, std::string_view value)
{
  ava::core::Error error(category, std::move(message));
  if (!field.empty())
    error.with_context(std::string(field), std::string(value));
  return error;
}

bool has_forbidden_path_byte(std::filesystem::path const& path)
{
  return ava::core::has_c0_or_del(path.string());
}

ava::core::VoidResult validate_limits(CommandLimits const& limits)
{
  auto const invalid = [](std::size_t value, std::size_t hard_max) { return value == 0 || value > hard_max; };
  if (invalid(limits.max_request_bytes, kHardMaxRequestBytes) || invalid(limits.max_argument_bytes, kHardMaxArgumentBytes) ||
      invalid(limits.max_argv_entries, kHardMaxArgvEntries) || invalid(limits.max_path_entries, kHardMaxPathEntries) ||
      invalid(limits.max_path_bytes, kHardMaxPathBytes) || invalid(limits.max_shebang_bytes, kHardMaxShebangBytes) ||
      invalid(limits.max_shebang_depth, kHardMaxShebangDepth))
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command limits must be non-zero and within fixed safety bounds"));
  }
  return {};
}

ava::core::Result<CompatibilityParse> parse_compatibility_command(std::string_view command, CommandLimits const& limits)
{
  if (auto valid = validate_limits(limits); !valid)
    return std::unexpected(std::move(valid.error()));
  if (command.empty())
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command request must not be empty"));
  if (command.size() > limits.max_request_bytes)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command request exceeds the bounded input size"));
  if (ava::core::has_c0_or_del(command))
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command request contains a NUL or forbidden control byte"));

  enum class Quote
  {
    None,
    Single,
    Double,
  };

  CompatibilityParse parsed;
  std::string current;
  Quote quote = Quote::None;
  bool token_started = false;

  auto append_current = [&]() -> ava::core::VoidResult {
    if (!token_started)
      return {};
    if (current.size() > limits.max_argument_bytes)
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command argument exceeds the bounded field size"));
    if (parsed.argv.size() >= limits.max_argv_entries)
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command request has too many argv entries"));
    parsed.argv.push_back(std::move(current));
    current.clear();
    token_started = false;
    return {};
  };

  for (std::size_t index = 0; index < command.size(); ++index)
  {
    char const ch = command[index];
    if (quote == Quote::Single)
    {
      if (ch == '\'')
        quote = Quote::None;
      else
        current.push_back(ch);  // POSIX single quotes preserve backslashes literally.
      token_started = true;
      continue;
    }
    if (quote == Quote::Double)
    {
      if (ch == '"')
      {
        quote = Quote::None;
        token_started = true;
        continue;
      }
      if (ch == '$' || ch == '`')
      {
        // Parameter/command expansion cannot be represented as direct argv.
        parsed.requires_raw_shell = true;
        current.push_back(ch);
        token_started = true;
        continue;
      }
      if (ch == '\\')
      {
        if (index + 1 >= command.size())
          return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command request contains an unterminated escape"));
        char const next = command[++index];
        // In double quotes only these four characters consume a backslash;
        // preserve it for every other character to keep argv semantics exact.
        if (next == '$' || next == '`' || next == '"' || next == '\\')
          current.push_back(next);
        else
        {
          current.push_back('\\');
          current.push_back(next);
        }
        token_started = true;
        continue;
      }
      current.push_back(ch);
      token_started = true;
      continue;
    }

    if (ch == '\\')
    {
      if (index + 1 >= command.size())
        return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command request contains an unterminated escape"));
      current.push_back(command[++index]);
      token_started = true;
      continue;
    }
    if (ch == '\'')
    {
      quote = Quote::Single;
      token_started = true;
      continue;
    }
    if (ch == '"')
    {
      quote = Quote::Double;
      token_started = true;
      continue;
    }
    if (is_shell_metacharacter(ch))
    {
      parsed.requires_raw_shell = true;
      current.push_back(ch);
      token_started = true;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch)) != 0)
    {
      if (auto appended = append_current(); !appended)
        return std::unexpected(std::move(appended.error()));
      continue;
    }
    current.push_back(ch);
    token_started = true;
  }

  if (quote != Quote::None)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command request contains an unterminated quote"));
  if (auto appended = append_current(); !appended)
    return std::unexpected(std::move(appended.error()));
  if (parsed.argv.empty())
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command request must encode a non-empty executable argument"));
  if (parsed.argv.front().empty())
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "command request has an ambiguous empty executable argument"));
  if (is_assignment_word(parsed.argv.front()) || is_shell_command_position_word(parsed.argv.front()))
    parsed.requires_raw_shell = true;
  return parsed;
}

ava::core::VoidResult validate_structured_argv(std::vector<std::string> const& argv, CommandLimits const& limits)
{
  if (auto valid = validate_limits(limits); !valid)
    return valid;
  if (argv.empty())
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "structured command argv must not be empty"));
  if (argv.size() > limits.max_argv_entries)
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "structured command has too many argv entries"));
  if (argv.front().empty())
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "structured command has an ambiguous empty executable argument"));

  std::size_t total_size = 0;
  for (auto const& argument : argv)
  {
    if (argument.size() > limits.max_argument_bytes)
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "structured command argument exceeds the bounded field size"));
    if (ava::core::has_c0_or_del(argument))
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "structured command argument contains a NUL or forbidden control byte"));
    if (exceeds_limit(total_size, argument.size(), limits.max_request_bytes))
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "structured command exceeds the bounded request size"));
    total_size += argument.size();
  }
  return {};
}

ava::core::VoidResult validate_raw_shell(std::string_view shell_text, CommandLimits const& limits)
{
  auto parsed = parse_compatibility_command(shell_text, limits);
  if (!parsed)
    return std::unexpected(std::move(parsed.error()));
  return {};
}

}  // namespace ava::command::detail

namespace ava::command {

CommandIntent::CommandIntent(CommandIntentLane lane, std::vector<std::string> argv, std::optional<std::filesystem::path> cwd, std::string source_text)
    : lane_(lane), argv_(std::move(argv)), cwd_(std::move(cwd)), source_text_(std::move(source_text))
{
}

ava::core::Result<CommandIntent> CommandIntent::compatibility(std::string command, CommandLimits limits)
{
  auto parsed = detail::parse_compatibility_command(command, limits);
  if (!parsed)
    return std::unexpected(std::move(parsed.error()));
  if (parsed->requires_raw_shell)
    return CommandIntent(CommandIntentLane::RawShell, {}, std::nullopt, std::move(command));
  return CommandIntent(CommandIntentLane::Compatibility, std::move(parsed->argv), std::nullopt, std::move(command));
}

ava::core::Result<CommandIntent> CommandIntent::structured(std::vector<std::string> argv, std::optional<std::filesystem::path> cwd, CommandLimits limits)
{
  if (auto valid = detail::validate_structured_argv(argv, limits); !valid)
    return std::unexpected(std::move(valid.error()));
  if (cwd && (cwd->empty() || detail::has_forbidden_path_byte(*cwd) || cwd->string().size() > limits.max_path_bytes))
  {
    return std::unexpected(detail::command_error(ava::core::ErrorCategory::InvalidArgument,
                                                 "structured command cwd must be non-empty, safe, and within its bound", "cwd", cwd->string()));
  }
  return CommandIntent(CommandIntentLane::StructuredArgv, std::move(argv), std::move(cwd), {});
}

ava::core::Result<CommandIntent> CommandIntent::raw_shell(std::string shell_text, CommandLimits limits)
{
  if (auto valid = detail::validate_raw_shell(shell_text, limits); !valid)
    return std::unexpected(std::move(valid.error()));
  return CommandIntent(CommandIntentLane::RawShell, {}, std::nullopt, std::move(shell_text));
}

CommandIntentLane CommandIntent::lane() const noexcept
{
  return lane_;
}

bool CommandIntent::has_argv() const noexcept
{
  return lane_ != CommandIntentLane::RawShell;
}

std::vector<std::string> const& CommandIntent::argv() const noexcept
{
  return argv_;
}

std::optional<std::filesystem::path> const& CommandIntent::requested_cwd() const noexcept
{
  return cwd_;
}

std::string_view CommandIntent::source_text() const noexcept
{
  return source_text_;
}

}  // namespace ava::command

#include "sys.h"
#include "ava/core/trusted_home.h"
#include "utils/AtomicFuzzyBool.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include "debug.h"      // ASSERT

namespace ava::core {
namespace {

// The frozen flag exists only in debug builds, as requested: it is the
// one-time gate that makes resolve_trusted_account() assert HOME is not read
// again after startup initialization.
//
// Transitions *once* from WasFalse to True: once True it stays True.
utils::AtomicFuzzyBool g_account_frozen(fuzzy::WasFalse);

// The resolved account cached for cached_trusted_account(). Populated by
// resolve_trusted_account() and then read-only for the rest of the process, so
// the trusted home is derived once and reused by every later command planner.
std::optional<TrustedAccount> g_cache;

// Read the trusted account directly from the environment and passwd database.
// Mirrors ava::config::home_dir(): prefer HOME, fall back to getpwuid_r only
// when HOME is unset or not absolute. The account name always comes from the
// passwd entry so the home path and USER/LOGNAME describe the same account.
Result<TrustedAccount> read_trusted_account_from_env()
{
  std::filesystem::path home;
  // The account name is always taken from the passwd database; HOME does not
  // carry a trustworthy user name.
  long const suggested = ::sysconf(_SC_GETPW_R_SIZE_MAX);
  std::vector<char> storage(static_cast<std::size_t>(suggested > 0 ? suggested : 16 * 1024));
  passwd record{};
  passwd* resolved = nullptr;
  int const status = ::getpwuid_r(::geteuid(), &record, storage.data(), storage.size(), &resolved);

  char const* const home_env = ::getenv("HOME");
  if (home_env != nullptr && home_env[0] != '\0')
  {
    auto home_path = std::filesystem::path(home_env).lexically_normal();
    if (home_path.is_absolute())
      home = std::move(home_path);
  }
  if (home.empty() && status == 0 && resolved != nullptr && resolved->pw_dir != nullptr && resolved->pw_dir[0] != '\0')
  {
    auto passwd_home = std::filesystem::path(resolved->pw_dir).lexically_normal();
    if (passwd_home.is_absolute())
      home = std::move(passwd_home);
  }

  if (home.empty())
    return std::unexpected(Error(ErrorCategory::Io, "failed to resolve the trusted home directory from HOME or the passwd database"));

  if (status != 0 || resolved == nullptr || !resolved->pw_name || resolved->pw_name[0] == '\0')
  {
    auto error = Error(ErrorCategory::Io, "failed to discover the trusted local account for command planning");
    if (status != 0)
      error.with_context("cause", std::strerror(status));
    return std::unexpected(std::move(error));
  }
  return TrustedAccount{.home = std::move(home), .user = std::string(resolved->pw_name)};
}

}  // namespace

// Resolve the trusted account by reading HOME (passwd fallback).
//
// This is the single function that reads the HOME environment variable.
// It may only be called once; which is asserted in debug builds, so the sensitive HOME
// value is never re-read after startup initialization. The resolved account is also
// stored for use with `cached_trusted_account` after the freeze.
Result<TrustedAccount> resolve_trusted_account()
{
  DoutEntering(dc::avacore|continued_cf, "resolve_trusted_account() -> ");

  auto account = read_trusted_account_from_env();
  if (!account)
  {
    Dout(dc::finish, account.error());
    return std::unexpected(std::move(account.error()));
  }
  g_cache = *account;

  // The environment variable HOME should *only* be read by `read_trusted_account_from_env`
  // and that function may only be called *once*. Therefore `read_trusted_account_from_env`
  // may only be called while g_account_frozen was still false. If this fires, the account was
  // read after being frozen; call load_account_once_and_freeze() once during startup and do not call
  // resolve_trusted_account() directly or repeatedly.
  ASSERT(g_account_frozen.is_momentary_false(std::memory_order::relaxed));

  Dout(dc::finish, *account);

  // Successfully determined trusted home and user.
  g_account_frozen.store(fuzzy::True, std::memory_order::release);

  return *account;
}

TrustedAccount const& cached_trusted_account()
{
  // The caller used the cached account before initialization completed; call load_account_once_and_freeze()
  // during startup and only call cached_trusted_account after that succeeds.
  ASSERT(g_account_frozen.is_true(std::memory_order::acquire));
  // The cache is empty even though the account is frozen; resolve_trusted_account must store into g_cache before
  // freezing, so check that initialization path.
  ASSERT(g_cache.has_value());

  return g_cache.value();
}

ava::core::VoidResult load_account_once_and_freeze()
{
  DoutEntering(dc::avacore, "load_account_once_and_freeze()");

  // No-op if already frozen.
  if (g_account_frozen.is_transitory_false())
  {
    // Transitory false means that g_account_frozen might have become True in the meantime.
    // In other words, another thread might have entered here before us. But only one thread
    // is allowed to call ava::core::resolve_trusted_account.
    static std::mutex m;        // This works because this is the ONLY place where ava::core::resolve_trusted_account is called.
    std::lock_guard lock(m);
    // If now g_account_frozen is still false then we are truly the first thread because
    // inside this critial area the WasFalse can not transition to True due to another thread.
    if (g_account_frozen.is_transitory_false())
    {
      auto resolved = ava::core::resolve_trusted_account();
      if (!resolved)
        return std::unexpected(std::move(resolved.error()));
    }
  }
  return {};
}

}  // namespace ava::core

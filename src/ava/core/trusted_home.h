#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <filesystem>
#include <optional>
#include <string>

namespace ava::core {

// The trusted local account: a home directory and account name resolved once
// at startup. The home directory is read from the HOME environment variable
// (falling back to the passwd database only when HOME is unset or not an
// absolute path), using the same precedence as ava::config::home_dir(). The
// account name always comes from the passwd database (getpwuid_r), because the
// trusted-home path and the command USER/LOGNAME must describe one real local
// account and the USER environment variable is not authoritative for that.
struct TrustedAccount
{
  std::filesystem::path home;
  ava::core::NoLogString user;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Called for every session from construct.
// Only reads HOME the first time that it is called; after that this is a no-op.
[[nodiscard]] ava::core::VoidResult load_account_once_and_freeze();

// Return the account resolved by a prior load_account_once_and_freeze call.
[[nodiscard]] TrustedAccount const& cached_trusted_account();

}  // namespace ava::core

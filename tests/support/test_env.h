#pragma once

#include "tests/support/test_harness.h"
#include "ava/config/provider_profiles.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace ava::tests {

// Owns a unique HOME/XDG tree and clears inherited provider credentials/endpoints
// (including built-in profile environment names) and proxy routing.
// Construct before starting workers and destroy after joining
// them: the POSIX environment is process-global. Declare scoped test overrides
// after this object so they restore before its environment and directory teardown.
// PATH, locale, loader/sanitizer options and timeout policy are deliberately kept.
class HermeticTestEnvironment
{
 public:
  explicit HermeticTestEnvironment(std::string_view prefix = "ava-env-")
      : directory_(ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), prefix))
  {
    // Snapshot names before unsetenv changes the environ array.
    auto const names = provider_and_proxy_names();
    for (auto const& name : names)
      variables_.emplace_back(name, std::nullopt);
    for (auto const& [name, subdirectory] :
         {std::pair{"HOME", "home"}, {"XDG_CONFIG_HOME", "config"}, {"XDG_STATE_HOME", "state"}, {"XDG_DATA_HOME", "data"}, {"XDG_CACHE_HOME", "cache"}})
    {
      auto const path = directory_.path() / subdirectory;
      std::filesystem::create_directory(path);
      variables_.emplace_back(name, path.string());
    }
  }

  HermeticTestEnvironment(HermeticTestEnvironment const&) = delete;
  HermeticTestEnvironment& operator=(HermeticTestEnvironment const&) = delete;
  HermeticTestEnvironment(HermeticTestEnvironment&&) = delete;
  HermeticTestEnvironment& operator=(HermeticTestEnvironment&&) = delete;

  ~HermeticTestEnvironment() noexcept
  {
    // Also remove provider/proxy variables introduced inside the fixture before
    // restoring the original snapshot. Never print their potentially secret values.
    try
    {
      for (auto const& name : provider_and_proxy_names())
        if (::unsetenv(name.c_str()) != 0)
          expect(false, "clear test environment variable " + name);
    }
    catch (...)
    {
      expect(false, "enumerate test environment variables during restoration");
    }
  }

  [[nodiscard]] std::filesystem::path const& root() const noexcept { return directory_.path(); }

 private:
  static std::vector<std::string> const& builtin_environment_names()
  {
    static std::vector<std::string> const names = [] {
      std::vector<std::string> result;
      for (auto const& profile : ava::config::builtin_provider_profiles())
      {
        if (!profile.default_base_url_env.empty())
          result.push_back(profile.default_base_url_env);
        if (!profile.api_key_env.empty())
          result.push_back(profile.api_key_env);
        // Bespoke built-ins use the auth loader's uppercase provider-ID key
        // convention even when their profile has no explicit api_key_env.
        std::string key = profile.provider_id;
        for (char& character : key)
        {
          if (character >= 'a' && character <= 'z')
            character = static_cast<char>(character - 'a' + 'A');
          else if (character == '-')
            character = '_';
        }
        result.push_back(key + "_API_KEY");
      }
      std::ranges::sort(result);
      result.erase(std::unique(result.begin(), result.end()), result.end());
      return result;
    }();
    return names;
  }

  static bool isolated_variable(std::string_view name)
  {
    if (name.starts_with("ANTHROPIC_") || name.starts_with("OPENAI_") || name.starts_with("GEMINI_") || name.starts_with("GOOGLE_"))
      return true;
    auto const& names = builtin_environment_names();
    if (std::ranges::find(names, name) != names.end())
      return true;
    return name == "http_proxy" || name == "https_proxy" || name == "all_proxy" || name == "no_proxy" || name == "HTTP_PROXY" || name == "HTTPS_PROXY" ||
           name == "ALL_PROXY" || name == "NO_PROXY";
  }

  static std::vector<std::string> provider_and_proxy_names()
  {
    std::vector<std::string> names;
    for (auto** entry = ::environ; entry && *entry; ++entry)
    {
      std::string_view const variable(*entry);
      auto const name = variable.substr(0, variable.find('='));
      if (isolated_variable(name))
        names.emplace_back(name);
    }
    return names;
  }

  ScopedTestDirectory directory_;
  // deque keeps nonmovable ScopedEnvVar objects in place without per-entry owners.
  std::deque<ScopedEnvVar> variables_;
};

}  // namespace ava::tests

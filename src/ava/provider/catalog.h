#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/config/model_config.h"
#include "ava/config/provider_config.h"
#include "ava/config/provider_profiles.h"
#include "ava/config/xdg_paths.h"
#include "ava/provider/registry.h"
#include "ava/core/result.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ava::provider {

// Immutable application-scoped provider composition. Owns the exact factories
// used to create runtime Provider instances plus the narrow descriptors needed
// for id/display/connect/auth/api-family metadata. Contains no secrets.
//
// Construct once before any session opens and pin the shared_ptr through
// OpenContext, SessionResources, and asynchronous work. Never rebuild from disk
// mid-session.
class ProviderCatalog
{
  class ConstructionKey
  {
    friend class ProviderCatalog;
    ConstructionKey() = default;

   public:
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

 public:
  ProviderCatalog(ProviderCatalog const&) = delete;
  ProviderCatalog& operator=(ProviderCatalog const&) = delete;
  ProviderCatalog(ProviderCatalog&&) = delete;
  ProviderCatalog& operator=(ProviderCatalog&&) = delete;

  // The private key keeps construction inside the validated factories while
  // allowing make_shared to own both the catalog and its control block.
  ProviderCatalog(ConstructionKey, ProviderRegistry registry, std::vector<ava::config::ProviderProfile> profiles,
                  std::vector<ava::config::UserProviderDefinition> user_definitions);

  // Compose built-in profiles/factories and load/validate providers.json.
  // Missing providers.json succeeds. Present unsafe/invalid file fails closed.
  // Resolves the six declarative generic built-in *_BASE_URL overrides once
  // under the providers.json URL policy and pins them into factories/profiles.
  [[nodiscard]] static ava::core::Result<std::shared_ptr<ProviderCatalog const>> build(ava::config::XdgPaths const& paths);

  // Built-ins only; no filesystem access and no generic *_BASE_URL env reads.
  // Pins compiled default endpoints. Intended for focused unit tests/fallbacks.
  // Production startup must use build() so env overrides are applied once.
  [[nodiscard]] static std::shared_ptr<ProviderCatalog const> build_builtins_only();

  [[nodiscard]] bool contains(std::string_view provider_id) const noexcept;
  [[nodiscard]] ava::core::Result<std::unique_ptr<Provider>> create(std::string_view provider_id) const;
  [[nodiscard]] std::vector<std::string> provider_ids() const;

  [[nodiscard]] std::span<ava::config::ProviderProfile const> profiles() const noexcept;
  [[nodiscard]] std::optional<ava::config::ProviderProfile> find_profile(std::string_view provider_id) const;
  [[nodiscard]] std::optional<ava::config::ProviderProfile> profile_for_model(ava::config::ModelInfo const& model) const;
  [[nodiscard]] std::optional<ava::config::ProviderProfile> reasoning_profile_for_model(ava::config::ModelInfo const& model) const;
  [[nodiscard]] std::string display_name(std::string_view provider_id) const;
  [[nodiscard]] bool accepts_reasoning_format(ava::config::ModelInfo const& model, std::string_view format) const;
  [[nodiscard]] ava::core::VoidResult validate_reasoning_request(ava::config::ModelInfo const& model, std::string_view level,
                                                                 std::optional<long long> budget_tokens, std::string_view display) const;

  // Active selection / configured default: provider must have a factory and any
  // declared model api_family must match the catalog descriptor.
  [[nodiscard]] ava::core::VoidResult validate_active_model(ava::config::ModelInfo const& model) const;

  // Listing diagnostics only: models whose provider lacks a factory remain visible.
  [[nodiscard]] bool provider_has_runtime_factory(std::string_view provider_id) const noexcept;

  [[nodiscard]] std::span<ava::config::UserProviderDefinition const> user_definitions() const noexcept;

  // True when the catalog descriptor declares explicit auth:none.
  [[nodiscard]] bool provider_auth_is_none(std::string_view provider_id) const noexcept;
  // Validated api_key_env for user-defined api_key providers; empty otherwise.
  [[nodiscard]] std::string provider_api_key_env(std::string_view provider_id) const;
  [[nodiscard]] bool provider_is_user_defined(std::string_view provider_id) const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  ProviderRegistry registry_;
  std::vector<ava::config::ProviderProfile> profiles_;
  std::vector<ava::config::UserProviderDefinition> user_definitions_;
};

// Ensure a catalog exists for adapters that have not yet been updated to inject
// one. Prefer an already-pinned pointer; otherwise build from paths.
[[nodiscard]] ava::core::Result<std::shared_ptr<ProviderCatalog const>> ensure_provider_catalog(std::shared_ptr<ProviderCatalog const> existing,
                                                                                                ava::config::XdgPaths const& paths);

}  // namespace ava::provider

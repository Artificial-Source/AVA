#pragma once

// Private helpers shared by the provider_config translation units.
// Not part of the public AVA config API.

#include "ava/debug/print_members_on.h"
#include "ava/config/provider_config.h"
#include "ava/core/error.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace ava::config::provider_config_detail {

using Json = nlohmann::json;

constexpr std::size_t kMaxProviderIdBytes = 64;
constexpr std::size_t kMaxDisplayNameBytes = 128;
constexpr std::size_t kMaxBaseUrlBytes = 2048;
constexpr std::size_t kMaxRequestPathBytes = 1024;
constexpr std::size_t kMaxApiKeyEnvBytes = 128;
constexpr std::size_t kMaxStrictJsonDepth = 8;

[[nodiscard]] ava::core::Error config_error(std::string message, std::string_view field = {});
[[nodiscard]] ava::core::Error path_error(ava::core::ErrorCategory category, std::string message, std::filesystem::path const& path,
                                          std::string_view operation = {});
[[nodiscard]] std::string errno_message();

[[nodiscard]] bool has_encoded_separator_or_dot_ambiguity(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_provider_id(std::string_view id) noexcept;
[[nodiscard]] bool is_valid_shell_env_name(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_display_name(std::string_view name) noexcept;
[[nodiscard]] bool is_unreserved_hostname_char(char ch) noexcept;
[[nodiscard]] bool is_valid_hostname(std::string_view host) noexcept;
[[nodiscard]] bool is_valid_ipv4(std::string_view host) noexcept;
[[nodiscard]] bool is_valid_ipv6_literal(std::string_view host) noexcept;
[[nodiscard]] bool is_localhost_host(std::string_view host) noexcept;
[[nodiscard]] bool path_has_dot_or_empty_segment(std::string_view path) noexcept;

[[nodiscard]] ava::core::VoidResult validate_request_path(std::string_view path, std::string_view field);

struct ParsedBaseUrl
{
  std::string canonical_base;  // scheme://host[:port][/path...] without trailing slashes

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] ava::core::Result<ParsedBaseUrl> parse_and_validate_base_url(std::string_view raw);
[[nodiscard]] std::string join_endpoint(std::string_view base_url, std::string_view request_path);

[[nodiscard]] ava::core::Result<ProviderProtocol> parse_protocol(Json const& value);
[[nodiscard]] ava::core::Result<ProviderAuthMode> parse_auth_mode(Json const& value);
[[nodiscard]] ava::core::VoidResult reject_unknown_fields(Json const& object, std::set<std::string> const& allowed, std::string_view context);
[[nodiscard]] ava::core::Result<UserProviderDefinition> parse_one_provider(Json const& object);

// nullopt => path does not exist. empty string => present zero-byte file.
[[nodiscard]] ava::core::Result<std::optional<std::string>> read_providers_file(std::filesystem::path const& path);

}  // namespace ava::config::provider_config_detail

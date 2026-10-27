#pragma once

#include "ava/config/auth.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ava::config {

struct AuthRecordMember
{
  std::string key;
  ava::core::NoLogString raw_value;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] bool is_valid_provider_id(std::string_view provider_id);
[[nodiscard]] ava::core::Result<std::string> provider_credential_object_json(ProviderCredential const& credential);
[[nodiscard]] ava::core::Result<std::vector<AuthRecordMember>> parse_auth_record_members(std::string_view text, std::filesystem::path const& path = {});
[[nodiscard]] std::string serialize_auth_record_members(std::vector<AuthRecordMember> const& members);

}  // namespace ava::config

#pragma once
#include "ava/http/transport.h"
#include "ava/provider/provider.h"
#include "ava/core/result.h"

#include <string_view>

namespace ava::provider {
struct OpenAIProviderOptions;
}  // namespace ava::provider

namespace ava::provider::detail {

[[nodiscard]] ava::core::Result<ava::http::HttpRequest> build_openai_responses_request(ProviderRequest const& request, std::string_view access_token,
                                                                                       OpenAIProviderOptions const& options, bool include_max_output_tokens);
[[nodiscard]] ava::core::VoidResult apply_openai_auth_options(ava::http::HttpRequest& request, ProviderAuthContext const& auth);

}  // namespace ava::provider::detail

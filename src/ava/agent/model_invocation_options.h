#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/config/model_config.h"
#include "ava/provider/provider.h"
#include "ava/core/NoLogString.h"

#include <optional>
#include <string>
#include <vector>

namespace ava::agent {

struct ModelInvocationOptions
{
  std::string provider_id = "openai";
  std::string model_id = "gpt-5.5";
  ava::core::NoLogString system_prompt = {};
  bool stream = true;
  bool supports_tools = true;
  bool supports_streaming = true;
  std::vector<std::string> input_modalities = {"text"};
  std::optional<long long> max_output_tokens = std::nullopt;
  std::optional<ava::provider::ProviderReasoningOptions> reasoning = std::nullopt;
  std::optional<ava::config::ModelPricing> pricing = std::nullopt;
  std::string api_family = {};
  std::string reasoning_format = {};
  std::vector<std::string> compatibility_quirks = {};

  // Prompt/context data is sensitive; never stream this aggregate through
  // generated debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::agent

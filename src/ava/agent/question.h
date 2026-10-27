#pragma once

#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::agent {

struct QuestionOption
{
  ava::core::NoLogString value;
  ava::core::NoLogString label;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct QuestionPrompt
{
  ava::core::NoLogString header;
  ava::core::NoLogString question;
  std::vector<QuestionOption> options;
  bool multiple = false;
  bool allow_custom = false;
  bool secret = false;
  bool modal = false;
  bool searchable = false;
  std::function<bool()> auto_resolve = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct QuestionAnswer
{
  std::vector<ava::core::NoLogString> selected_options;
  ava::core::NoLogString custom_text;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using QuestionResolver = std::function<ava::core::Result<QuestionAnswer>(QuestionPrompt const&)>;

[[nodiscard]] ava::core::Result<QuestionPrompt> parse_question_prompt(std::string_view arguments_json, std::string_view tool_name);
[[nodiscard]] std::string serialize_question_answer_result(QuestionPrompt const& prompt, QuestionAnswer const& answer);

}  // namespace ava::agent

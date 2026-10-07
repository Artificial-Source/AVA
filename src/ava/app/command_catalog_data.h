#pragma once

#include "ava/debug/print_members_on.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ava::app {

// Application-owned, frontend-neutral command catalog records. The app/TUI
// integration seam explicitly projects their presentation data into TUI DTOs.
struct SlashCommandArgumentCompletionRecord
{
  std::string value = {};
  std::string display_label = {};
  std::string description = {};
  std::string category = {};
  std::vector<std::string> required_previous_args = {};
  std::size_t argument_index = 0;
  bool append_space = true;
  bool enabled = true;
  std::string disabled_reason = "";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SlashCommandCatalogItem
{
  std::string command;
  std::string display_label = {};
  std::string description;
  std::string hint = "";
  std::string category = "";
  std::vector<std::string> aliases = {};
  std::string key_display = "";
  bool enabled = true;
  std::string disabled_reason = "";
  std::vector<SlashCommandArgumentCompletionRecord> argument_completions = {};
  bool argument_completion = false;
  std::string completion_insert_text = "";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct FileReferenceCatalogItem
{
  std::string value;
  std::string description;
  std::string category = "Files";
  bool directory = false;
  bool enabled = true;
  std::string disabled_reason = "";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::app

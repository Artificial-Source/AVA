#include "sys.h"
#include "serialization_detail.h"
#include "serialization_json.h"
#include "ava/config/model_config.h"
#include "ava/provider/catalog.h"
#include "ava/provider/registry.h"
#include "ava/core/json.h"

namespace ava::app::rpc::detail {
namespace {

std::string reasoning_level_map_json(std::vector<ava::config::ModelReasoningLevelMapping> const& mappings)
{
  std::string json = "{";
  for (std::size_t index = 0; index < mappings.size(); ++index)
  {
    if (index > 0)
      json += ',';
    json += '"';
    json += ava::core::json::escape(mappings[index].level);
    json += "\":";
    if (!mappings[index].supported)
    {
      json += "null";
    }
    else if (mappings[index].provider_level)
    {
      json += '"';
      json += ava::core::json::escape(*mappings[index].provider_level);
      json += '"';
    }
    else
    {
      json += "true";
    }
  }
  json += '}';
  return json;
}

}  // namespace

std::string SessionResultSerializer::model_info_json(ava::config::ModelInfo const& model, bool configured) const
{
  bool const registered = ava::provider::ProviderCatalog::build_builtins_only()->contains(model.provider_id);
  std::string json = "{";
  json += string_field_json("provider", model.provider_id);
  json += ',';
  json += string_field_json("model", model.model_id);
  json += ',';
  json += string_field_json("display_name", model.display_name);
  json += ',';
  json += string_field_json("family", model.family);
  json += ',';
  json += string_field_json("api_family", model.api_family);
  json += ',';
  json += bool_field_json("registered", registered);
  json += ',';
  json += bool_field_json("selectable", registered && configured);
  append_optional_integer(json, "context_window_tokens", model.context_window_tokens);
  append_optional_integer(json, "max_output_tokens", model.max_output_tokens);
  append_optional_bool(json, "supports_tools", model.supports_tools);
  append_optional_bool(json, "supports_streaming", model.supports_streaming);
  append_optional_bool(json, "supports_reasoning", model.supports_reasoning);
  append_optional_bool(json, "reports_usage", model.reports_usage);
  json += ",\"input_modalities\":";
  json += string_array_json(model.input_modalities);
  json += ",\"output_modalities\":";
  json += string_array_json(model.output_modalities);
  json += ",\"reasoning_levels\":";
  json += string_array_json(ava::config::supported_reasoning_levels(model));
  json += ",\"raw_reasoning_levels\":";
  json += string_array_json(model.reasoning_levels);
  json += ",\"reasoning_level_map\":";
  json += reasoning_level_map_json(model.reasoning_level_mappings);
  if (!model.reasoning_format.empty())
  {
    json += ',';
    json += string_field_json("reasoning_format", model.reasoning_format);
  }
  json += ",\"compatibility_quirks\":";
  json += string_array_json(model.compatibility_quirks);
  json += ',';
  runtime::ModelSelection_ts::crat model_selection_r = session_.model_selection_r();
  json += bool_field_json("selected", model_selection_r->model.provider_id == model.provider_id && model_selection_r->model.model_id == model.model_id);
  json += '}';
  return json;
}

}  // namespace ava::app::rpc::detail

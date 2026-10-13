#include "sys.h"
#include "ava/app/runtime_catalog.h"
#include "ava/provider/catalog.h"

#include <algorithm>
#include <string>
#include <utility>

namespace ava::app {
namespace {

std::string model_key(std::string_view provider_id, std::string_view model_id)
{
  return std::string(provider_id) + "/" + std::string(model_id);
}

ava::core::Result<std::shared_ptr<ava::provider::ProviderCatalog const>> session_catalog(runtime::session_ts::crat const& session_r)
{
  if (session_r->provider_catalog())
    return session_r->provider_catalog();
  return ava::provider::ProviderCatalog::build(session_r->paths());
}

}  // namespace

ava::core::Result<std::unique_ptr<ava::provider::Provider>> create_runtime_provider(runtime::session_ts const& unlocked_session, std::string_view provider_id)
{
  SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);

  auto catalog = session_r->provider_catalog();
  if (!catalog)
  {
    auto built = ava::provider::ProviderCatalog::build(session_r->paths());
    if (!built)
      return std::unexpected(std::move(built.error()));
    catalog = std::move(*built);
  }
  return catalog->create(provider_id);
}

ava::core::Result<std::unique_ptr<ava::provider::Provider>> create_runtime_provider(std::string_view provider_id)
{
  // Compatibility seam for call sites that have not yet threaded a session/catalog.
  // Prefer create_runtime_provider(session, id).
  return ava::provider::ProviderCatalog::build_builtins_only()->create(provider_id);
}

ava::core::Result<std::vector<ava::config::ModelInfo>> runtime_model_catalog(runtime::session_ts::crat const& session_r)
{
  auto catalog = session_catalog(session_r);
  if (!catalog)
    return std::unexpected(std::move(catalog.error()));
  auto registry = ava::config::load_model_registry(session_r->paths());
  if (!registry)
    return std::unexpected(std::move(registry.error()));
  std::vector<ava::config::ModelInfo> registered;
  for (auto const& model : registry->models)
    if ((*catalog)->contains(model.provider_id))
      registered.push_back(model);

  runtime::ModelSelection_ts::crat model_selection_r = session_r->model_selection_r();
  if (!model_selection_r->scoped_model_cycle)
    return registered;
  std::vector<ava::config::ModelInfo> scoped;
  scoped.reserve(model_selection_r->scoped_model_cycle->size());
  for (auto const& id : *model_selection_r->scoped_model_cycle)
  {
    auto found = std::ranges::find_if(registered, [&](auto const& model) { return id == model_key(model.provider_id, model.model_id); });
    if (found != registered.end())
      scoped.push_back(*found);
  }
  return scoped;
}

ava::core::Result<ava::config::ModelInfo> select_runtime_model(runtime::session_ts::crat const& session_r, std::optional<std::string_view> provider_id,
                                                               std::string_view model_id)
{
  if (model_id.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "model is required"));
  if (provider_id && !provider_id->empty())
    return resolve_runtime_model(session_r->paths(), session_r->provider_catalog(), *provider_id, model_id);
  if (auto current =
          resolve_runtime_model(session_r->paths(), session_r->provider_catalog(), session_r->model_selection_r()->model.provider_id, model_id);
      current)
    return current;

  auto models = runtime_model_catalog(session_r);
  if (!models)
    return std::unexpected(std::move(models.error()));
  std::vector<ava::config::ModelInfo> matches;
  for (auto const& model : *models)
    if (model.model_id == model_id)
      matches.push_back(model);
  if (matches.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::NotFound, "model is not configured");
    error.with_context("model", std::string(model_id));
    return std::unexpected(std::move(error));
  }
  if (matches.size() != 1)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "model id is ambiguous; provider is required");
    error.with_context("model", std::string(model_id));
    error.with_context("matches", std::to_string(matches.size()));
    return std::unexpected(std::move(error));
  }
  return matches.front();
}

ava::core::Result<ava::config::ModelInfo> cycle_runtime_model(runtime::session_ts::crat const& session_r, int direction)
{
  auto models = runtime_model_catalog(session_r);
  if (!models)
    return std::unexpected(std::move(models.error()));
  if (models->empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::NotFound, "no registered provider models are enabled for cycling"));
  std::size_t selected = direction < 0 ? models->size() - 1 : 0;
  runtime::ModelSelection_ts::crat model_selection_r = session_r->model_selection_r();
  for (std::size_t index = 0; index < models->size(); ++index)
  {
    if ((*models)[index].provider_id == model_selection_r->model.provider_id && (*models)[index].model_id == model_selection_r->model.model_id)
    {
      selected = direction < 0 ? (index == 0 ? models->size() - 1 : index - 1) : (index + 1) % models->size();
      break;
    }
  }
  return (*models)[selected];
}

}  // namespace ava::app

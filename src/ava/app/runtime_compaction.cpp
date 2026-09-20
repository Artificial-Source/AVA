#include "sys.h"
#include "ava/event/events.h"
#include "ava/http/curl_transport.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_compaction.h"
#include "ava/app/runtime_credentials.h"
#include "ava/app/runtime_json.h"
#include "ava/app/runtime_retry.h"
#include "ava/provider/catalog.h"
#include "ava/provider/registry.h"

#include <mutex>
#include <optional>
#include <tuple>
#include <utility>

namespace ava::app {
namespace {

ava::event::RuntimeEventMetadata runtime_event_metadata_1(std::string_view session_id)
{
  return ava::event::RuntimeEventMetadata{
      .timestamp = ava::session::now_timestamp(),
      .session_id = std::string(session_id),
  };
}

// Build retry policy for a compaction provider request using an owned session id.
//
// Retry callbacks can outlive any session access guard and therefore capture no Session reference.
ava::core::Error agent_loop_canceled_error()
{
  return ava::core::Error(ava::core::ErrorCategory::Unknown, "agent loop canceled");
}

ava::http::RetryOptions compaction_retry_options(std::string session_id, runtime::RunOptions const& options)
{
  ava::http::RetryOptions retry_options;
  retry_options.cancel_requested = options.cancel_requested;
  retry_options.observation = {.observation = options.observation, .context = options.trace_context};
  retry_options.response_retry_decision = ava::provider::provider_retry_decision;
  retry_options.on_retry = [session_id = std::move(session_id), &options](ava::http::RetryOptions::Event const& retry) {
    ava::event::RetryPayload payload;
    if (retry.status_code > 0)
      payload.text = "HTTP status " + std::to_string(retry.status_code);
    payload.status = retry.streaming ? "streaming" : "request";
    payload.trigger = "provider_transport";
    payload.reason = retry.reason;
    payload.attempt = retry.attempt;
    payload.max_attempts = retry.max_attempts;
    payload.delay_ms = retry.delay_ms;
    payload.remaining_ms = retry.remaining_ms;
    auto metadata = runtime_event_metadata_1(session_id);
    if (retry.countdown_tick)
    {
      return ava::event::emit_event(
          options.event_sink, ava::event::RuntimeEvent{std::move(metadata), ava::event::RetryTickEvent{.payload = std::move(payload), .diagnostics = {}}});
    }
    return ava::event::emit_event(options.event_sink,
                                  ava::event::RuntimeEvent{std::move(metadata), ava::event::RetryEvent{.payload = std::move(payload), .diagnostics = {}}});
  };
  return retry_options;
}

}  // namespace

ava::core::Error stale_compaction_snapshot_error(std::string_view trigger, std::size_t snapshot_entries, std::size_t current_entries)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session changed during context compaction after retry");
  error.with_context("trigger", std::string(trigger));
  error.with_context("snapshot_entries", std::to_string(snapshot_entries));
  error.with_context("current_entries", std::to_string(current_entries));
  return error;
}

ava::core::Result<ava::session::CompactionConfig> resolve_compaction_config_impl(ava::config::XdgPaths const& paths,
                                                                                 std::shared_ptr<ava::provider::ProviderCatalog const> const& provider_catalog,
                                                                                 ava::config::ModelInfo const& current_model,
                                                                                 ava::session::CompactionConfig config)
{
  if (!config.model_explicit)
  {
    config.provider_id = current_model.provider_id;
    config.model_id = current_model.model_id;
    return config;
  }
  auto const provider_id = config.provider_explicit ? config.provider_id : current_model.provider_id;
  auto const model_id = config.model_id;
  auto model = resolve_runtime_model(paths, provider_catalog, provider_id, model_id);
  if (!model)
  {
    model.error().with_context("compaction_provider", provider_id).with_context("compaction_model", model_id);
    return std::unexpected(std::move(model.error()));
  }
  config.provider_id = model->provider_id;
  config.model_id = model->model_id;
  return config;
}

ava::core::Result<ava::session::CompactionConfig> resolve_compaction_config(runtime::session_ts const& unlocked_session, ava::session::CompactionConfig config)
{
  auto [paths, provider_catalog, current_model] = [&] {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    return std::tuple{session_r->paths(), session_r->provider_catalog(), session_r->model()};
  }();
  return resolve_compaction_config_impl(paths, provider_catalog, current_model, std::move(config));
}

ava::core::Result<PreparedCompactionContext> prepare_compaction_context(std::vector<ava::session::SessionEntry> const& entries,
                                                                        ava::session::CompactionConfig const& config,
                                                                        std::vector<std::string> const& replayed_user_messages)
{
  return ava::agent::prepare_compaction_context(entries, config, replayed_user_messages);
}

ava::core::Result<std::string> build_compaction_summary_prompt(std::vector<ava::session::SessionEntry> const& entries,
                                                               ava::session::CompactionConfig const& config, std::string_view instructions,
                                                               std::size_t estimated_tokens)
{
  return ava::agent::build_compaction_summary_prompt(entries, config, instructions, estimated_tokens);
}

ava::core::Result<std::string> generate_compaction_summary_impl(
    ava::config::XdgPaths const& paths, std::shared_ptr<ava::provider::ProviderCatalog const> const& provider_catalog,
    ava::config::ModelInfo const& current_model, bool offline, std::string session_id, std::optional<ava::process::ProcessScopeV1> const& session_process_scope,
    std::vector<ava::session::SessionEntry> const& entries, ava::session::CompactionConfig const& config, std::string_view instructions,
    std::size_t estimated_tokens, ava::provider::Provider const& provider, ava::http::Transport& transport, runtime::RunOptions const& options)
{
  if (offline || options.offline)
  {
    return std::unexpected(offline_provider_error("compact"));
  }
  auto effective_config = resolve_compaction_config_impl(paths, provider_catalog, current_model, config);
  if (!effective_config)
    return std::unexpected(std::move(effective_config.error()));
  ava::core::Result<ava::config::ModelInfo> summary_model =
      effective_config->model_explicit ? resolve_runtime_model(paths, provider_catalog, effective_config->provider_id, effective_config->model_id)
                                       : current_model;
  if (!summary_model)
    return std::unexpected(std::move(summary_model.error()));

  auto summary_options = options;
  std::unique_ptr<ava::provider::Provider> owned_provider;
  ava::provider::Provider const* summary_provider = &provider;
  if (effective_config->provider_id != current_model.provider_id)
  {
    summary_options.access_token.clear();
    summary_options.credential_type = "bearer";
    summary_options.openai_oauth = false;
    summary_options.openai_account_id.clear();
    if (!session_process_scope)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Configuration, "compaction process authority is unavailable"));
    }
    ava::http::CurlCliTransport auth_transport(*session_process_scope);
    auto prepared =
        prepare_runtime_credentials(paths, effective_config->provider_id, std::move(summary_options), auth_transport, "compaction", provider_catalog);
    if (!prepared)
      return std::unexpected(std::move(prepared.error()));
    summary_options = std::move(*prepared);
    auto catalog = provider_catalog;
    if (!catalog)
    {
      auto built = ava::provider::ensure_provider_catalog(nullptr, paths);
      if (!built)
        return std::unexpected(std::move(built.error()));
      catalog = std::move(*built);
    }
    auto created = catalog->create(effective_config->provider_id);
    if (!created)
      return std::unexpected(std::move(created.error()));
    owned_provider = std::move(*created);
    summary_provider = owned_provider.get();
  }
  if (summary_options.access_token.empty() && summary_options.credential_type != "none")
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "compaction requires provider access token");
    error.with_context("provider", effective_config->provider_id);
    return std::unexpected(std::move(error));
  }

  std::optional<ava::http::RetryTransport> retry_transport;
  ava::http::Transport* summary_transport = &transport;
  if (summary_options.enable_transport_retries)
  {
    retry_transport.emplace(transport, compaction_retry_options(session_id, summary_options));
    summary_transport = &*retry_transport;
    summary_options.enable_transport_retries = false;
  }
  // Compaction is a provider request in the same runtime turn, so observe its
  // logical request with the already-established run/turn context. Do not
  // construct the decorator on the disabled path.
  std::optional<ava::http::ObservedTransport> observed_transport;
  if (summary_options.observation && summary_options.observation->enabled())
  {
    try
    {
      observed_transport.emplace(*summary_transport,
                                 ava::http::TransportObservation{.observation = summary_options.observation, .context = summary_options.trace_context});
      summary_transport = &*observed_transport;
    }
    catch (...)
    {
      summary_options.observation->account_external_failure();
    }
  }

  return ava::agent::generate_context_compaction_summary(
      entries, *effective_config, instructions, estimated_tokens, *summary_provider, *summary_transport,
      ava::agent::ContextCompactionInvocation{
          .model = ava::agent::ModelInvocationOptions{.provider_id = effective_config->provider_id,
                                                      .model_id = effective_config->model_id,
                                                      .stream = summary_options.openai_oauth,
                                                      .supports_tools = false,
                                                      .supports_streaming = summary_model->supports_streaming.value_or(true),
                                                      .max_output_tokens = summary_model->max_output_tokens,
                                                      .compatibility_quirks = summary_model->compatibility_quirks},
          .access_token = summary_options.access_token,
          .credential_type = summary_options.credential_type,
          .openai_oauth = summary_options.openai_oauth,
          .openai_account_id = summary_options.openai_account_id,
          .cancel_requested = summary_options.cancel_requested});
}

ava::core::Result<std::string> generate_compaction_summary(runtime::session_ts const& unlocked_session, std::vector<ava::session::SessionEntry> const& entries,
                                                           ava::session::CompactionConfig const& config, std::string_view instructions,
                                                           std::size_t estimated_tokens, ava::provider::Provider const& provider,
                                                           ava::http::Transport& transport, runtime::RunOptions const& options)
{
  auto [paths, provider_catalog, current_model, offline, session_id, session_process_scope] = [&] {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    return std::tuple{session_r->paths(),      session_r->provider_catalog(), session_r->model(),
                      session_r->is_offline(), session_r->store.session_id(), session_r->session_process_scope()};
  }();
  return generate_compaction_summary_impl(paths, provider_catalog, current_model, offline, std::move(session_id), session_process_scope, entries, config,
                                          instructions, estimated_tokens, provider, transport, options);
}

}  // namespace ava::app

namespace ava::app::runtime {

ava::core::Result<bool> compact_runtime_context(session_ts& unlocked_session, ava::session::SessionReadAuthority read_authority, std::string_view trigger,
                                                ava::provider::Provider const& provider, ava::http::Transport& transport, RunOptions const& options,
                                                std::vector<std::string> const& replayed_user_messages)
{
  if (options.access_token.empty() && options.credential_type != "none")
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "compaction requires provider access token"));
  }

  auto [paths, provider_catalog, current_model, offline, session_id, session_process_scope] = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::tuple{session_r->paths(),      session_r->provider_catalog(), session_r->model(),
                      session_r->is_offline(), session_r->store.session_id(), session_r->session_process_scope()};
  }();

  auto loaded_config = ava::session::load_compaction_config(paths);
  if (!loaded_config)
    return std::unexpected(std::move(loaded_config.error()));
  auto config = resolve_compaction_config_impl(paths, provider_catalog, current_model, std::move(*loaded_config));
  if (!config)
    return std::unexpected(std::move(config.error()));

  constexpr std::size_t max_compaction_attempts = 2;
  auto const trigger_text = std::string(trigger);
  std::size_t last_snapshot_entries = 0;
  std::size_t last_current_entries = 0;
  bool context_retry_event_emitted = false;
  for (std::size_t attempt = 0; attempt < max_compaction_attempts; ++attempt)
  {
    if (options.cancel_requested && options.cancel_requested())
    {
      return std::unexpected(agent_loop_canceled_error());
    }

    ava::core::Result<std::vector<ava::session::SessionEntry>> entries =
        std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "session entries were not loaded"));
    entries = read_authority.load();
    if (!entries)
      return std::unexpected(std::move(entries.error()));

    auto prepared = prepare_compaction_context(*entries, *config, replayed_user_messages);
    if (!prepared)
      return std::unexpected(std::move(prepared.error()));
    auto const threshold = ava::session::effective_auto_threshold_tokens(*config, current_model.context_window_tokens);
    std::size_t estimated_tokens = prepared->estimated_tokens;
    std::size_t threshold_tokens = threshold;
    if (trigger == "auto")
    {
      auto decision = ava::session::should_auto_compact(*entries, *config, current_model.context_window_tokens);
      if (!decision)
        return std::unexpected(std::move(decision.error()));
      if (!decision->should_compact)
        return false;
      estimated_tokens = decision->estimated_tokens;
      threshold_tokens = decision->threshold_tokens;
    }

    if (trigger == "context_overflow" && !context_retry_event_emitted)
    {
      ava::event::RetryPayload retry_payload;
      retry_payload.status = "started";
      retry_payload.trigger = trigger_text;
      retry_payload.reason = "context_overflow";
      retry_payload.attempt = 1;
      retry_payload.max_attempts = 1;
      ava::event::RetryDiagnostics retry_diagnostics;
      retry_diagnostics.estimated_tokens = estimated_tokens;
      retry_diagnostics.threshold_tokens = threshold_tokens;
      if (auto emitted = ava::event::emit_event(
              options.event_sink,
              ava::event::RuntimeEvent{runtime_event_metadata_1(session_id),
                                       ava::event::RetryEvent{.payload = std::move(retry_payload), .diagnostics = std::move(retry_diagnostics)}});
          !emitted)
      {
        return std::unexpected(std::move(emitted.error()));
      }
      context_retry_event_emitted = true;
    }

    ava::event::CompactionPayload start_payload;
    start_payload.provider = config->provider_id;
    start_payload.model = config->model_id;
    start_payload.status = "started";
    start_payload.trigger = trigger_text;
    start_payload.reason = trigger == "auto" ? "automatic" : trigger == "context_overflow" ? "overflow" : "manual";
    start_payload.attempt = attempt + 1;
    start_payload.max_attempts = max_compaction_attempts;
    start_payload.estimated_tokens = estimated_tokens;
    start_payload.threshold_tokens = threshold_tokens;
    start_payload.retained_tokens = prepared->retained_tokens;
    if (auto emitted = ava::event::emit_event(
            options.event_sink,
            ava::event::RuntimeEvent{runtime_event_metadata_1(session_id), ava::event::CompactionStartEvent{.payload = std::move(start_payload)}});
        !emitted)
    {
      return std::unexpected(std::move(emitted.error()));
    }

    auto summary = generate_compaction_summary_impl(paths, provider_catalog, current_model, offline, session_id, session_process_scope,
                                                    prepared->active_entries, *config, "", estimated_tokens, provider, transport, options);
    if (!summary)
      return std::unexpected(std::move(summary.error()));
    if (options.cancel_requested && options.cancel_requested())
    {
      return std::unexpected(agent_loop_canceled_error());
    }

    auto entry = ava::session::make_manual_compaction_entry(ava::session::ManualCompactionRequest{.summary = *summary,
                                                                                                  .instructions = "",
                                                                                                  .config = *config,
                                                                                                  .estimated_tokens = estimated_tokens,
                                                                                                  .threshold_tokens = threshold_tokens,
                                                                                                  .retained_tokens = prepared->retained_tokens,
                                                                                                  .trigger = trigger_text,
                                                                                                  .recent_context = prepared->recent_context,
                                                                                                  .recent_context_omitted = prepared->recent_context_omitted});
    if (!entry)
      return std::unexpected(std::move(entry.error()));
    if (!options.active_compaction_append_route)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "active compaction append route is unavailable"));
    }

    auto const snapshot_entries = entries->size();
    auto appended = options.active_compaction_append_route(std::move(*entry), std::move(*entries), options.cancel_requested);
    if (!appended)
      return std::unexpected(std::move(appended.error()));
    bool const snapshot_stale = *appended == ava::session::SessionCompactionAppendResult::SnapshotMismatch;
    if (!snapshot_stale)
    {
      ava::event::CompactionPayload end_payload;
      end_payload.provider = config->provider_id;
      end_payload.model = config->model_id;
      end_payload.status = "completed";
      end_payload.trigger = trigger_text;
      end_payload.reason = trigger == "auto" ? "automatic" : trigger == "context_overflow" ? "overflow" : "manual";
      end_payload.attempt = attempt + 1;
      end_payload.max_attempts = max_compaction_attempts;
      end_payload.estimated_tokens = estimated_tokens;
      end_payload.threshold_tokens = threshold_tokens;
      end_payload.retained_tokens = prepared->retained_tokens;
      end_payload.post_compaction_tokens = ava::session::estimate_tokens(*summary) + prepared->retained_tokens;
      end_payload.summary_bytes = summary->size();
      if (auto emitted = ava::event::emit_event(
              options.event_sink,
              ava::event::RuntimeEvent{runtime_event_metadata_1(session_id), ava::event::CompactionEndEvent{.payload = std::move(end_payload)}});
          !emitted)
      {
        return std::unexpected(std::move(emitted.error()));
      }
      return true;
    }
    last_snapshot_entries = snapshot_entries;
    auto current_entries = read_authority.load();
    if (!current_entries)
      return std::unexpected(std::move(current_entries.error()));
    last_current_entries = current_entries->size();
    if (attempt + 1 < max_compaction_attempts)
    {
      ava::event::RetryPayload retry_payload;
      retry_payload.status = "started";
      retry_payload.trigger = trigger_text;
      retry_payload.reason = "stale_compaction_snapshot";
      retry_payload.attempt = attempt + 2;
      retry_payload.max_attempts = max_compaction_attempts;
      ava::event::RetryDiagnostics retry_diagnostics;
      retry_diagnostics.snapshot_entries = last_snapshot_entries;
      retry_diagnostics.current_entries = last_current_entries;
      if (auto emitted = ava::event::emit_event(
              options.event_sink,
              ava::event::RuntimeEvent{runtime_event_metadata_1(session_id),
                                       ava::event::RetryEvent{.payload = std::move(retry_payload), .diagnostics = std::move(retry_diagnostics)}});
          !emitted)
      {
        return std::unexpected(std::move(emitted.error()));
      }
    }
  }
  return std::unexpected(stale_compaction_snapshot_error(trigger_text, last_snapshot_entries, last_current_entries));
}

}  // namespace ava::app::runtime

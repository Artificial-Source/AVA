#include "sys.h"
#include "ava/event/events.h"
#include "ava/http/curl_transport.h"
#include "ava/app/command_registry.h"
#include "ava/app/commands.h"
#include "ava/app/rpc/input.h"
#include "ava/app/rpc/output.h"
#include "ava/app/rpc/prompt_worker.h"
#include "ava/app/rpc/protocol.h"
#include "ava/app/rpc/resolvers.h"
#include "ava/app/rpc/run_state.h"
#include "ava/app/rpc/serialization.h"
#include "ava/app/rpc/serialization_json.h"
#include "ava/app/rpc/session_commands.h"
#include "ava/app/rpc/session_operators.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/signal_policy.h"
#include "ava/session/attachments.h"
#include "ava/provider/catalog.h"
#include "ava/provider/provider_utils.h"
#include "ava/provider/registry.h"
#include "ava/core/thread.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <istream>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <signal.h>
#include <unistd.h>
#include "debug.h"
#ifdef CWDEBUG
#include "ava/debug/print_reference.h"
#endif

namespace ava::app {
namespace {

std::filesystem::path resolve_rpc_attachment_path(std::filesystem::path const& current_dir, std::string const& attachment_path)
{
  auto path = std::filesystem::path(attachment_path);
  if (path.is_absolute())
    return path;
  return current_dir / path;
}

int rpc_base64_value(char ch)
{
  if (ch >= 'A' && ch <= 'Z')
    return ch - 'A';
  if (ch >= 'a' && ch <= 'z')
    return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9')
    return ch - '0' + 52;
  if (ch == '+')
    return 62;
  if (ch == '/')
    return 63;
  return -1;
}

ava::core::Result<std::string> decode_rpc_image_base64(std::string_view data)
{
  if (!ava::provider::is_valid_base64(data))
  {
    return std::unexpected(rpc::invalid_rpc("RPC image upload data must be base64"));
  }
  std::size_t padding = 0;
  if (!data.empty() && data.back() == '=')
  {
    ++padding;
    if (data.size() >= 2 && data[data.size() - 2] == '=')
      ++padding;
  }
  auto const decoded_size = (data.size() / 4U) * 3U - padding;
  if (decoded_size == 0 || decoded_size > ava::session::kMaxImageAttachmentBytes)
  {
    auto error = rpc::invalid_rpc("RPC image upload byte size is invalid");
    error.with_context("max_bytes", std::to_string(ava::session::kMaxImageAttachmentBytes));
    error.with_context("byte_size", std::to_string(decoded_size));
    return std::unexpected(std::move(error));
  }

  std::string bytes;
  bytes.reserve(decoded_size);
  for (std::size_t index = 0; index < data.size(); index += 4U)
  {
    auto const a = rpc_base64_value(data[index]);
    auto const b = rpc_base64_value(data[index + 1]);
    auto const c = data[index + 2] == '=' ? 0 : rpc_base64_value(data[index + 2]);
    auto const d = data[index + 3] == '=' ? 0 : rpc_base64_value(data[index + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0)
    {
      return std::unexpected(rpc::invalid_rpc("RPC image upload data must be base64"));
    }
    auto const value = static_cast<unsigned int>((a << 18) | (b << 12) | (c << 6) | d);
    bytes.push_back(static_cast<char>((value >> 16) & 0xFFU));
    if (data[index + 2] != '=')
      bytes.push_back(static_cast<char>((value >> 8) & 0xFFU));
    if (data[index + 3] != '=')
      bytes.push_back(static_cast<char>(value & 0xFFU));
  }
  return bytes;
}

bool is_direct_run_command_type(std::string_view type)
{
  return type == "run_command" || type == "run_bash";
}

struct RpcDirectCommandWorkerOptions
{
  runtime::session_ts& unlocked_session;
  rpc::output_ts& output;
  rpc::RpcRunState& run_state;
  rpc::PendingResolverState& pending_state;
  runtime::RunOptions runtime_options;
  std::string request_id;
  std::string command;
};

struct RpcCompactionWorkerOptions
{
  runtime::session_ts& unlocked_session;
  rpc::output_ts& output;
  rpc::RpcRunState& run_state;
  ava::provider::Provider const& injected_provider;
  std::string injected_provider_id;
  ava::http::Transport& transport;
  ava::http::Transport& auth_transport;
  runtime::RunOptions runtime_options;
  std::string request_id;
  std::optional<std::string> instructions;
};

ava::core::JoinThread make_rpc_direct_command_worker(RpcDirectCommandWorkerOptions options)
{
  return ava::core::JoinThread::create("rpc_direct_command", [options = std::move(options)](std::stop_token stop_token) mutable {
    auto finish = [&](ava::core::Result<std::string> result) {
      auto const reason = (stop_token.stop_requested() || rpc::cancel_requested(options.run_state)) ? std::string_view("canceled")
                                                                                                    : std::string_view("run_completed_before_safe_point");
      auto cleared = rpc::begin_terminal_publication(options.run_state);
      ava::core::VoidResult written;
      if (result)
        written = rpc::write_success(options.output, options.request_id, *result);
      else
        written = rpc::write_error(options.output, options.request_id, result.error());
      if (written)
        written = rpc::write_skipped_queue_events(options.output, options.unlocked_session, cleared, reason);
      if (written)
        written = rpc::write_follow_up_errors(options.output, options.run_state, cleared.follow_up_messages, reason);
      rpc::complete_terminal_publication(options.run_state, options.request_id);
      if (!written)
        rpc::record_async_error(options.run_state, std::move(written.error()));
    };

    auto policy_permission_resolver = options.runtime_options.permission_resolver;
    auto base_cancel_requested = options.runtime_options.cancel_requested;
    auto command_cancel_requested = [&run_state = options.run_state, stop_token, base_cancel_requested] {
      return stop_token.stop_requested() || rpc::cancel_requested(run_state) || (base_cancel_requested && base_cancel_requested());
    };

    auto permission_resolver = rpc::make_rpc_permission_resolver(options.pending_state, options.output, options.run_state, options.unlocked_session,
                                                                 std::move(policy_permission_resolver), options.request_id);

    ava::event::EventBus event_bus;
    rpc::subscribe_event_envelope_writer(event_bus, options.output);
    auto result = run_command(options.unlocked_session,
                              CommandRequest{.command = "/bash " + options.command,
                                             .event_sink = ava::event::make_runtime_event_bus_adapter(event_bus, rpc::rpc_event_context(options.request_id)),
                                             .permission_resolver = std::move(permission_resolver),
                                             .cancel_requested = std::move(command_cancel_requested)});
    if (!result)
    {
      finish(std::unexpected(std::move(result.error())));
      return;
    }
    if (!result->handled)
    {
      auto error = rpc::invalid_rpc("command not found");
      error.with_context("type", "run_command");
      finish(std::unexpected(std::move(error)));
      return;
    }
    finish(rpc::command_result_json(*result));
  });
}

ava::core::JoinThread make_rpc_compaction_worker(RpcCompactionWorkerOptions options)
{
  return ava::core::JoinThread::create("rpc_compaction", [options = std::move(options)](std::stop_token stop_token) mutable {
    auto finish = [&](ava::core::Result<std::string> result) {
      static_cast<void>(rpc::begin_terminal_publication(options.run_state));
      ava::core::VoidResult written;
      if (result)
        written = rpc::write_success(options.output, options.request_id, *result);
      else
        written = rpc::write_error(options.output, options.request_id, result.error());
      rpc::complete_terminal_publication(options.run_state, options.request_id);
      if (!written)
        rpc::record_async_error(options.run_state, std::move(written.error()));
    };

    ava::config::XdgPaths paths;
    std::string provider_id;
    std::shared_ptr<ava::provider::ProviderCatalog const> prompt_catalog;
    bool session_offline = false;
    ava::core::Result<rpc::ProviderHandle> selected_provider = std::unexpected(rpc::invalid_rpc("compact provider was not selected"));
    ava::core::VoidResult config_valid = {};
    {
      runtime::session_ts& unlocked_session = options.unlocked_session;
      CRITICAL_AREA_BEGIN_R(session);
      paths = session_r->paths();
      session_offline = session_r->is_offline();
      prompt_catalog = session_r->provider_catalog();
      CRITICAL_AREA_END_R(session);
      auto loaded_config = ava::session::load_compaction_config(paths);
      if (!loaded_config)
      {
        config_valid = std::unexpected(std::move(loaded_config.error()));
      }
      else
      {
        auto config = resolve_compaction_config(unlocked_session, std::move(*loaded_config));
        if (!config)
        {
          config_valid = std::unexpected(std::move(config.error()));
        }
        else
        {
          provider_id = config->provider_id;
          if (provider_id == runtime::session_ts::rat(unlocked_session)->model().provider_id)
          {
            selected_provider = rpc::provider_for_session_model(unlocked_session, options.injected_provider_id, options.injected_provider);
          }
          else
          {
            auto catalog = prompt_catalog ? prompt_catalog : ava::provider::ProviderCatalog::build_builtins_only();
            auto provider = catalog->create(provider_id);
            if (!provider)
              selected_provider = std::unexpected(std::move(provider.error()));
            else
              selected_provider = rpc::ProviderHandle{.provider = nullptr, .owned = std::move(*provider)};
          }
        }
      }
    }
    if (!config_valid)
    {
      finish(std::unexpected(std::move(config_valid.error())));
      return;
    }
    if (session_offline)
    {
      finish(std::unexpected(offline_provider_error("compact")));
      return;
    }
    if (!selected_provider)
    {
      finish(std::unexpected(std::move(selected_provider.error())));
      return;
    }

    auto compact_runtime_options = rpc::ensure_prompt_runtime_options(paths, provider_id, std::move(options.runtime_options), options.auth_transport, "compact",
                                                                      std::move(prompt_catalog));
    if (!compact_runtime_options)
    {
      finish(std::unexpected(std::move(compact_runtime_options.error())));
      return;
    }

    auto base_cancel_requested = compact_runtime_options->cancel_requested;
    auto compact_cancel_requested = [&run_state = options.run_state, stop_token, base_cancel_requested] {
      return stop_token.stop_requested() || rpc::cancel_requested(run_state) || (base_cancel_requested && base_cancel_requested());
    };
    compact_runtime_options->cancel_requested = compact_cancel_requested;

    ava::event::EventBus event_bus;
    rpc::subscribe_event_envelope_writer(event_bus, options.output);
    auto summary_generator =
        CompactionSummaryGenerator([&](std::vector<ava::session::SessionEntry> const& entries, ava::session::CompactionConfig const& config,
                                       std::string_view instructions, std::size_t estimated_tokens) {
          return generate_compaction_summary(options.unlocked_session, entries, config, instructions, estimated_tokens, selected_provider->get(),
                                             options.transport, *compact_runtime_options);
        });
    std::string slash_command = "/compact";
    if (options.instructions)
      slash_command += " " + *options.instructions;
    auto command_result = run_command(options.unlocked_session,
                                      CommandRequest{
                                          .command = std::move(slash_command),
                                          .event_sink = ava::event::make_runtime_event_bus_adapter(event_bus, rpc::rpc_event_context(options.request_id)),
                                          .permission_resolver = compact_runtime_options->permission_resolver,
                                          .compaction_summary_generator = std::move(summary_generator),
                                          .cancel_requested = std::move(compact_cancel_requested),
                                          .propagate_compaction_errors = true,
                                      });
    if (!command_result)
    {
      finish(std::unexpected(std::move(command_result.error())));
      return;
    }
    finish(rpc::command_result_json(*command_result));
  });
}

}  // namespace

ava::core::VoidResult run_rpc_loop_impl(runtime::session_ts& unlocked_session, runtime::OpenContext const& open_context,
                                        ava::provider::Provider const& provider, ava::http::Transport& transport, ava::http::Transport& auth_transport,
                                        runtime::RunOptions runtime_options, rpc::RpcLineReader& input, std::ostream& out, ModeSignalLatch* signal_latch)
{
#ifdef CWDEBUG
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    DoutEntering(dc::rpc, "run_rpc_loop(...) with session_id=" << session_r->store.session_id() << ", provider_id=" << session_r->model().provider_id
                                                               << ", model_id=" << session_r->model().model_id);
  }
  AVA_ASSERT_SESSION_UNLOCKED(unlocked_session, "calling run_rpc_loop");
#endif

  rpc::RpcRunState run_state;
  rpc::PendingResolverState pending_state;
  rpc::output_ts output(out, [&] {
    // Request run cancellation and wake input before resolving pending requests. The resolver
    // cancellation then obtains output -> pending_state, matching publication gates.
    static_cast<void>(rpc::close_input_and_cancel(run_state));
    input.cancel();
    static_cast<void>(rpc::cancel_pending_resolvers(output, pending_state));
  });
  std::optional<ava::core::JoinThread> prompt_worker;
  std::optional<std::jthread> signal_watcher;
  if (signal_latch)
  {
    signal_watcher.emplace([&](std::stop_token stop_token) {
      while (!stop_token.stop_requested())
      {
        if (signal_latch->poll())
        {
          static_cast<void>(rpc::close_input_and_cancel(run_state));
          input.cancel();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
  }

  // At this point we are still single-threaded.
  CRITICAL_AREA_BEGIN_R(session);

  runtime_options.offline = runtime_options.offline || session_r->is_offline() || open_context.offline;
  if (!runtime_options.permission_resolver)
    runtime_options.permission_resolver = build_headless_permission_resolver(HeadlessPermissionPolicyOptions{});
  runtime_options.permission_resolver =
      ava::permissions::build_persistent_permission_rule_resolver(session_r->permission_rule_store(), std::move(runtime_options.permission_resolver));
  auto const injected_provider_id = session_r->model().provider_id;

  CRITICAL_AREA_END_R(session);

  auto reap_finished_prompt = [&] {
    if (prompt_worker && rpc::async_worker_reap_ready(run_state))
      prompt_worker.reset();
  };

  std::string line;
  std::optional<ava::core::Error> input_read_error;
  while (true)
  {
    // Publish terminal input state from the callback without waiting for the output mutex: a prompt
    // worker may hold it while flushing its terminal response. Resolver cancellation follows only
    // after read_line returns, preserving output -> pending-state lock order without delaying EOF.
    auto read_line = input.read_line(line, [&run_state](rpc::RpcInputTerminalOutcome outcome) { rpc::observe_input_terminal(run_state, outcome); });
    if (rpc::input_closed(run_state))
      static_cast<void>(rpc::cancel_pending_resolvers(output, pending_state));
    if (!read_line)
    {
      if (read_line.error().category() == ava::core::ErrorCategory::InvalidArgument)
      {
        if (auto written = rpc::write_error(output, "", read_line.error()); !written)
          return written;
        if (rpc::input_closed(run_state))
          break;
        continue;
      }
      input_read_error = std::move(read_line.error());
      break;
    }
    if (!*read_line)
      break;
    reap_finished_prompt();
    if (auto async_error = rpc::take_async_error(run_state))
      return std::unexpected(std::move(*async_error));
    auto command = parse_rpc_command_line(line);
    if (!command)
    {
      if (auto written = rpc::write_error(output, rpc::parse_error_response_id(line), command.error()); !written)
      {
        return written;
      }
      continue;
    }
    if (auto valid_version = rpc::validate_protocol_version(*command); !valid_version)
    {
      if (auto written = rpc::write_error(output, command->id, valid_version.error()); !written)
        return written;
      continue;
    }
    if (!rpc::is_rpc_command_type(command->type))
    {
      auto error = rpc::invalid_rpc("unknown RPC command type");
      error.with_context("type", command->type);
      if (auto written = rpc::write_error(output, command->id, error); !written)
        return written;
      continue;
    }
    auto const admission = rpc::await_command_admission(run_state, command->id, command->type != "cancel");
    if (admission == rpc::RpcCommandAdmission::InputClosed)
      break;
    if (admission == rpc::RpcCommandAdmission::DuplicateRequestId)
    {
      if (auto written = rpc::write_error(output, command->id, rpc::duplicate_request_id_error(command->id)); !written)
        return written;
      continue;
    }

    if (command->type == "get_protocol")
    {
      if (auto written = rpc::write_success(output, command->id, rpc::rpc_protocol_result_json()); !written)
      {
        return written;
      }
      continue;
    }

    auto session_command = rpc::handle_session_rpc_command(rpc::RpcSessionCommandContext{
        .command = *command, .unlocked_session = unlocked_session, .open_context = open_context, .output = output, .run_state = run_state});
    if (!session_command)
      return std::unexpected(std::move(session_command.error()));
    if (*session_command)
      continue;

    if (command->type == "permission_grants")
    {
      if (auto written = rpc::write_success(output, command->id, rpc::permission_session_grants_result_json(pending_state)); !written)
      {
        return written;
      }
      continue;
    }

    if (command->type == "permission_grant_revoke")
    {
      if (!command->grant_id || command->grant_id->empty())
      {
        if (auto written = rpc::write_error(output, command->id, rpc::invalid_rpc("permission_grant_revoke requires grant_id")); !written)
        {
          return written;
        }
        continue;
      }
      auto revoked = rpc::permission_session_grant_revoke_result_json(pending_state, *command->grant_id);
      if (!revoked)
      {
        if (auto written = rpc::write_error(output, command->id, revoked.error()); !written)
          return written;
        continue;
      }
      auto envelope = rpc::resolver_event_envelope("permission_grant_revoked", command->id, command->id, rpc::session_id_snapshot(unlocked_session), *revoked);
      if (auto written = rpc::Output::write_record(output, ava::event::serialize_event_envelope_jsonl(envelope)); !written)
        return written;
      if (auto written = rpc::write_success(output, command->id, *revoked); !written)
        return written;
      continue;
    }

    if (command->type == "permission_grants_clear")
    {
      auto const cleared = rpc::permission_session_grants_clear_result_json(pending_state);
      auto envelope = rpc::resolver_event_envelope("permission_grants_cleared", command->id, command->id, rpc::session_id_snapshot(unlocked_session), cleared);
      if (auto written = rpc::Output::write_record(output, ava::event::serialize_event_envelope_jsonl(envelope)); !written)
        return written;
      if (auto written = rpc::write_success(output, command->id, cleared); !written)
        return written;
      continue;
    }

    if (command->type == "list_commands")
    {
      if (rpc::active_run(run_state))
      {
        if (auto written = rpc::write_error(output, command->id, rpc::active_run_reject_error(command->type)); !written)
        {
          return written;
        }
        continue;
      }
      auto registry = load_command_registry(
          unlocked_session, CommandRegistryOptions{.include_builtins = true,
                                                   .include_prompt_commands = true,
                                                   .include_skills = true,
                                                   .include_plugin_commands = true,
                                                   .include_mcp_prompts = true,
                                                   .permission_resolver = runtime_options.permission_resolver,
                                                   .cancel_requested = [&] { return run_state.cancel_requested.load(std::memory_order_relaxed); }});
      if (auto written = rpc::write_success(output, command->id, rpc::command_registry_result_json(registry)); !written)
      {
        return written;
      }
      continue;
    }

    if (command->type == "invoke_command")
    {
      if (!command->name || command->name->empty())
      {
        if (auto written = rpc::write_error(output, command->id, rpc::invalid_rpc("invoke_command requires name")); !written)
        {
          return written;
        }
        continue;
      }
      if (rpc::active_run(run_state))
      {
        if (auto written = rpc::write_error(output, command->id, rpc::active_run_reject_error(command->type)); !written)
        {
          return written;
        }
        continue;
      }

      std::string slash_command = command->name->starts_with('/') ? *command->name : "/" + *command->name;
      if (command->command_arguments && !command->command_arguments->empty())
      {
        slash_command += " " + *command->command_arguments;
      }

      ava::event::EventBus event_bus;
      rpc::subscribe_event_envelope_writer(event_bus, output);
      std::optional<std::string> prompt_message;
      ava::config::XdgPaths paths;
      {
        paths = runtime::session_ts::rat(unlocked_session)->paths();
        auto result = run_command(unlocked_session,
                                  CommandRequest{.command = std::move(slash_command),
                                                 .event_sink = ava::event::make_runtime_event_bus_adapter(event_bus, rpc::rpc_event_context(command->id)),
                                                 .permission_resolver = runtime_options.permission_resolver});
        if (!result)
        {
          if (auto written = rpc::write_error(output, command->id, result.error()); !written)
            return written;
          continue;
        }
        if (!result->handled)
        {
          auto error = rpc::invalid_rpc("command not found");
          error.with_context("name", *command->name);
          if (auto written = rpc::write_error(output, command->id, error); !written)
            return written;
          continue;
        }
        if (result->prompt_message)
        {
          prompt_message = *result->prompt_message;
        }
        else
        {
          if (auto written = rpc::write_success(output, command->id, rpc::command_result_json(*result)); !written)
          {
            return written;
          }
          continue;
        }
      }

      reap_finished_prompt();
      rpc::set_active_run(run_state, rpc::RpcRunKind::Prompt, command->id);
      prompt_worker.emplace(rpc::make_rpc_prompt_worker(rpc::RpcPromptWorkerOptions{
          .unlocked_session = unlocked_session,
          .output = output,
          .run_state = run_state,
          .pending_state = pending_state,
          .injected_provider = provider,
          .injected_provider_id = injected_provider_id,
          .transport = transport,
          .auth_transport = auth_transport,
          .runtime_options = runtime_options,
          .paths = std::move(paths),
          .request_id = command->id,
          .message = std::move(*prompt_message),
      }));
      continue;
    }

    if (is_direct_run_command_type(command->type))
    {
      if (!command->command || command->command->empty())
      {
        if (auto written = rpc::write_error(output, command->id, rpc::invalid_rpc(command->type + " requires command")); !written)
        {
          return written;
        }
        continue;
      }
      if (rpc::active_run(run_state))
      {
        if (auto written = rpc::write_error(output, command->id, rpc::active_run_reject_error(command->type)); !written)
        {
          return written;
        }
        continue;
      }

      reap_finished_prompt();
      rpc::set_active_run(run_state, rpc::RpcRunKind::DirectCommand, command->id);
      prompt_worker.emplace(make_rpc_direct_command_worker(RpcDirectCommandWorkerOptions{.unlocked_session = unlocked_session,
                                                                                         .output = output,
                                                                                         .run_state = run_state,
                                                                                         .pending_state = pending_state,
                                                                                         .runtime_options = runtime_options,
                                                                                         .request_id = command->id,
                                                                                         .command = *command->command}));
      continue;
    }

    if (command->type == "permission_reply")
    {
      if (!command->request_id || command->request_id->empty())
      {
        if (auto written = rpc::write_error(output, command->id, rpc::invalid_rpc(command->type + " requires request_id")); !written)
        {
          return written;
        }
        continue;
      }
      if (!command->decision)
      {
        if (auto written = rpc::write_error(output, command->id, rpc::invalid_rpc("permission_reply requires decision")); !written)
        {
          return written;
        }
        continue;
      }
      if (!command->correlation_id || command->correlation_id->empty())
      {
        if (auto written = rpc::write_error(output, command->id, rpc::invalid_rpc("permission_reply requires correlation_id")); !written)
        {
          return written;
        }
        continue;
      }
      auto resolved = rpc::resolve_permission_reply(pending_state, *command->request_id, *command->correlation_id, *command->decision, command->reason);
      if (!resolved)
      {
        if (auto written = rpc::write_error(output, command->id, resolved.error()); !written)
          return written;
        continue;
      }
      auto envelope =
          rpc::resolver_event_envelope("permission_replied", *command->correlation_id, *command->correlation_id, rpc::session_id_snapshot(unlocked_session),
                                       rpc::permission_reply_payload_json(*command->request_id, *command->decision, command->reason));
      if (auto written = rpc::Output::write_record(output, ava::event::serialize_event_envelope_jsonl(envelope)); !written)
        return written;
      if (auto written = rpc::write_success(output, command->id, "{}"); !written)
        return written;
      continue;
    }

    if (command->type == "question_reply")
    {
      if (!command->request_id || command->request_id->empty())
      {
        if (auto written = rpc::write_error(output, command->id, rpc::invalid_rpc(command->type + " requires request_id")); !written)
        {
          return written;
        }
        continue;
      }
      if (!command->correlation_id || command->correlation_id->empty())
      {
        if (auto written = rpc::write_error(output, command->id, rpc::invalid_rpc("question_reply requires correlation_id")); !written)
        {
          return written;
        }
        continue;
      }
      auto resolved = rpc::resolve_question_reply(pending_state, *command->request_id, *command->correlation_id, command->answer, command->selected,
                                                  command->selected_options);
      if (!resolved)
      {
        if (auto written = rpc::write_error(output, command->id, resolved.error()); !written)
          return written;
        continue;
      }
      auto envelope =
          rpc::resolver_event_envelope("question_replied", *command->correlation_id, *command->correlation_id, rpc::session_id_snapshot(unlocked_session),
                                       rpc::question_reply_payload_json(*command->request_id, command->answer, command->selected, command->selected_options));
      if (auto written = rpc::Output::write_record(output, ava::event::serialize_event_envelope_jsonl(envelope)); !written)
        return written;
      if (auto written = rpc::write_success(output, command->id, "{}"); !written)
        return written;
      continue;
    }

    if (command->type == "steer")
    {
      if (!command->message)
      {
        if (auto written = rpc::write_error(output, command->id, rpc::invalid_rpc("steer requires message")); !written)
        {
          return written;
        }
        continue;
      }
      auto queued = rpc::queue_rpc_message(run_state.steering_messages, run_state, command->type, command->id, *command->message);
      if (!queued)
      {
        if (auto written = rpc::write_error(output, command->id, queued.error()); !written)
          return written;
        continue;
      }
      if (auto written = rpc::write_queue_event(output, unlocked_session, "steer_queued", *queued); !written)
      {
        return written;
      }
      std::string json = "{";
      json += rpc::bool_field_json("queued", true);
      json += ',';
      json += rpc::string_field_json("correlation_id", queued->correlation_id);
      json += '}';
      if (auto written = rpc::write_success(output, command->id, json); !written)
        return written;
      continue;
    }

    if (command->type == "follow_up")
    {
      if (!command->message)
      {
        if (auto written = rpc::write_error(output, command->id, rpc::invalid_rpc("follow_up requires message")); !written)
        {
          return written;
        }
        continue;
      }
      auto queued = rpc::queue_rpc_message(run_state.follow_up_messages, run_state, command->type, command->id, *command->message);
      if (!queued)
      {
        if (auto written = rpc::write_error(output, command->id, queued.error()); !written)
          return written;
        continue;
      }
      if (auto written = rpc::write_queue_event(output, unlocked_session, "follow_up_queued", *queued); !written)
      {
        return written;
      }
      continue;
    }

    if (command->type == "cancel")
    {
      auto cancellation = rpc::begin_cancellation(run_state);
      static_cast<void>(rpc::cancel_pending_resolvers(output, pending_state));
      if (auto run_controller = runtime::session_ts::wat(unlocked_session)->run_controller())
        static_cast<void>(run_controller->request_stop(StopReason::UserCanceled));
      if (cancellation.deferred_to_terminal_publication)
      {
        rpc::wait_for_terminal_publication(run_state);
      }
      else
      {
        if (auto written = rpc::write_skipped_queue_events(output, unlocked_session, cancellation.cleared, "canceled"); !written)
          return written;
      }

      auto const cleared_steering_count = cancellation.cleared.steering_messages.size() + cancellation.deferred_steering_count;
      auto const cleared_follow_up_count = cancellation.cleared.follow_up_messages.size() + cancellation.deferred_follow_up_count;
      auto cancel_event = rpc::resolver_event_envelope(
          "cancel_requested", command->id, cancellation.active_request_id.empty() ? command->id : cancellation.active_request_id,
          rpc::session_id_snapshot(unlocked_session),
          rpc::cancel_requested_payload_json(cancellation.active_run, cleared_steering_count, cleared_follow_up_count, cancellation.active_request_id));
      if (auto written = rpc::Output::write_record(output, ava::event::serialize_event_envelope_jsonl(cancel_event)); !written)
        return written;
      std::string json = "{";
      json += rpc::bool_field_json("cancel_requested", true);
      json += ',';
      json += rpc::bool_field_json("active_run", cancellation.active_run);
      json += ',';
      json += rpc::number_field_json("cleared_steer", cleared_steering_count);
      json += ',';
      json += rpc::number_field_json("cleared_follow_up", cleared_follow_up_count);
      json += '}';
      if (auto written = rpc::write_success(output, command->id, json); !written)
        return written;
      if (!cancellation.deferred_to_terminal_publication)
      {
        if (auto written = rpc::write_follow_up_errors(output, run_state, cancellation.cleared.follow_up_messages, "canceled"); !written)
          return written;
      }
      continue;
    }

    if (command->type == "compact")
    {
      reap_finished_prompt();
      bool admitted = false;
      {
        std::unique_lock lock(run_state.mutex);
        run_state.publication_cv.wait(lock, [&] { return !run_state.terminal_publication_in_progress || run_state.input_closed; });
        if (run_state.active_run_kind == rpc::RpcRunKind::None)
        {
          run_state.active_run_kind = rpc::RpcRunKind::Compaction;
          run_state.active_request_id = command->id;
          run_state.outstanding_request_ids.insert(command->id);
          run_state.cancel_requested.store(false, std::memory_order_relaxed);
          admitted = true;
        }
      }
      if (!admitted)
      {
        if (auto written = rpc::write_error(output, command->id, rpc::active_run_reject_error(command->type)); !written)
          return written;
        continue;
      }
      prompt_worker.emplace(make_rpc_compaction_worker(RpcCompactionWorkerOptions{
          .unlocked_session = unlocked_session,
          .output = output,
          .run_state = run_state,
          .injected_provider = provider,
          .injected_provider_id = injected_provider_id,
          .transport = transport,
          .auth_transport = auth_transport,
          .runtime_options = runtime_options,
          .request_id = command->id,
          .instructions = command->instructions,
      }));
      continue;
    }

    if (command->type == "context" || command->type == "export" || command->type == "export_html" || rpc::is_plugin_rpc_command(command->type) ||
        rpc::is_mcp_rpc_command(command->type))
    {
      if (rpc::active_run(run_state))
      {
        if (auto written = rpc::write_error(output, command->id, rpc::active_run_reject_error(command->type)); !written)
          return written;
        continue;
      }

      std::string slash_command;
      if (command->type == "context")
      {
        slash_command = "/context";
      }
      else if (command->type == "export")
      {
        slash_command = "/export";
      }
      else if (command->type == "export_html")
      {
        slash_command = "/export html";
        if (command->output_path && !command->output_path->empty())
          slash_command += " " + *command->output_path;
      }
      else if (rpc::is_plugin_rpc_command(command->type))
      {
        auto plugin_command = rpc::plugin_rpc_slash_command(*command);
        if (!plugin_command)
        {
          if (auto written = rpc::write_error(output, command->id, plugin_command.error()); !written)
            return written;
          continue;
        }
        slash_command = std::move(*plugin_command);
      }
      else
      {
        auto mcp_command = rpc::mcp_rpc_slash_command(*command);
        if (!mcp_command)
        {
          if (auto written = rpc::write_error(output, command->id, mcp_command.error()); !written)
            return written;
          continue;
        }
        slash_command = std::move(*mcp_command);
      }

      ava::event::EventBus event_bus;
      rpc::subscribe_event_envelope_writer(event_bus, output);
      auto result =
          run_command(unlocked_session, CommandRequest{.command = std::move(slash_command),
                                                       .event_sink = ava::event::make_runtime_event_bus_adapter(event_bus, rpc::rpc_event_context(command->id)),
                                                       .permission_resolver = runtime_options.permission_resolver});
      if (!result)
      {
        if (auto written = rpc::write_error(output, command->id, result.error()); !written)
          return written;
        continue;
      }
      if (auto written = rpc::write_success(output, command->id, rpc::command_result_json(*result)); !written)
        return written;
      continue;
    }

    if (command->type == "prompt")
    {
      if (!command->message)
      {
        if (auto written = rpc::write_error(output, command->id, rpc::invalid_rpc("prompt requires message")); !written)
        {
          return written;
        }
        continue;
      }

      if (rpc::active_run(run_state))
      {
        if (auto written = rpc::write_error(output, command->id, rpc::active_run_reject_error(command->type)); !written)
        {
          return written;
        }
        continue;
      }

      reap_finished_prompt();
      ava::config::XdgPaths paths;
      std::vector<ava::session::ImageAttachmentRef> image_attachments;
      bool attachment_import_failed = false;
      std::filesystem::path current_dir;
      {
        SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);
        paths = session_w->paths();
        current_dir = session_w->current_dir().empty() ? session_w->workspace_dir() : session_w->current_dir();
        image_attachments.reserve((command->attachments ? command->attachments->size() : 0U) + (command->images ? command->images->size() : 0U));
        if (command->attachments)
        {
          for (auto const& attachment_path : *command->attachments)
          {
            auto imported = ava::session::import_image_attachment(session_w->store, resolve_rpc_attachment_path(current_dir, attachment_path));
            if (!imported)
            {
              if (auto written = rpc::write_error(output, command->id, imported.error()); !written)
              {
                return written;
              }
              attachment_import_failed = true;
              break;
            }
            image_attachments.push_back(std::move(*imported));
          }
        }
        if (!attachment_import_failed && command->images)
        {
          for (auto const& image : *command->images)
          {
            auto bytes = decode_rpc_image_base64(image.data_base64);
            if (!bytes)
            {
              if (auto written = rpc::write_error(output, command->id, bytes.error()); !written)
              {
                return written;
              }
              attachment_import_failed = true;
              break;
            }
            auto imported = ava::session::import_image_attachment_bytes(session_w->store, *bytes, std::string_view(image.mime_type));
            if (!imported)
            {
              if (auto written = rpc::write_error(output, command->id, imported.error()); !written)
              {
                return written;
              }
              attachment_import_failed = true;
              break;
            }
            image_attachments.push_back(std::move(*imported));
          }
        }
      }
      if (attachment_import_failed)
      {
        continue;
      }
      auto prompt_runtime_options = runtime_options;
      prompt_runtime_options.image_attachments.clear();
      rpc::set_active_run(run_state, rpc::RpcRunKind::Prompt, command->id);

      prompt_worker.emplace(rpc::make_rpc_prompt_worker(rpc::RpcPromptWorkerOptions{
          .unlocked_session = unlocked_session,
          .output = output,
          .run_state = run_state,
          .pending_state = pending_state,
          .injected_provider = provider,
          .injected_provider_id = injected_provider_id,
          .transport = transport,
          .auth_transport = auth_transport,
          .runtime_options = std::move(prompt_runtime_options),
          .paths = std::move(paths),
          .request_id = command->id,
          .message = *command->message,
          .image_attachments = std::move(image_attachments),
      }));
      continue;
    }

    auto error = rpc::invalid_rpc("unknown RPC command type");
    error.with_context("type", command->type);
    if (auto written = rpc::write_error(output, command->id, error); !written)
      return written;
  }

  if (prompt_worker)
  {
    auto cleared = rpc::close_input_and_cancel(run_state);
    static_cast<void>(rpc::cancel_pending_resolvers(output, pending_state));
    if (auto written = rpc::write_skipped_queue_events(output, unlocked_session, cleared, "canceled"); !written)
    {
      return written;
    }
    if (auto written = rpc::write_follow_up_errors(output, run_state, cleared.follow_up_messages, "canceled"); !written)
    {
      return written;
    }
    prompt_worker.reset();
  }
  if (auto async_error = rpc::take_async_error(run_state))
    return std::unexpected(std::move(*async_error));
  if (input_read_error)
    return std::unexpected(std::move(*input_read_error));

  return {};
}

ava::core::VoidResult run_rpc_loop(runtime::session_ts& unlocked_session, runtime::OpenContext const& open_context, ava::provider::Provider const& provider,
                                   ava::http::Transport& transport, ava::http::Transport& auth_transport, runtime::RunOptions runtime_options,
                                   rpc::RpcLineReader& input, std::ostream& out)
{
  return run_rpc_loop_impl(unlocked_session, open_context, provider, transport, auth_transport, std::move(runtime_options), input, out, nullptr);
}

ava::core::VoidResult run_rpc_loop(runtime::session_ts& unlocked_session, runtime::OpenContext const& open_context, ava::provider::Provider const& provider,
                                   ava::http::Transport& transport, ava::http::Transport& auth_transport, runtime::RunOptions runtime_options, std::istream& in,
                                   std::ostream& out, rpc::RpcInputWake wake)
{
  rpc::StreamRpcLineReader input(in, std::move(wake));
  return run_rpc_loop(unlocked_session, open_context, provider, transport, auth_transport, std::move(runtime_options), input, out);
}

ava::core::VoidResult run_rpc_loop(runtime::session_ts& unlocked_session, runtime::OpenContext const& open_context, ava::provider::Provider const& provider,
                                   ava::http::Transport& transport, runtime::RunOptions runtime_options, std::istream& in, std::ostream& out,
                                   rpc::RpcInputWake wake)
{
  return run_rpc_loop(unlocked_session, open_context, provider, transport, transport, std::move(runtime_options), in, out, std::move(wake));
}

ava::core::VoidResult run_rpc_loop(runtime::session_ts& unlocked_session, runtime::OpenContext const& open_context, ava::provider::Provider const& provider,
                                   ava::http::Transport& transport, ava::http::Transport& auth_transport, runtime::RunOptions runtime_options, std::istream& in,
                                   std::ostream& out)
{
  return run_rpc_loop(unlocked_session, open_context, provider, transport, auth_transport, std::move(runtime_options), in, out, rpc::RpcInputWake{});
}

ava::core::VoidResult run_rpc_loop(runtime::session_ts& unlocked_session, runtime::OpenContext const& open_context, ava::provider::Provider const& provider,
                                   ava::http::Transport& transport, runtime::RunOptions runtime_options, std::istream& in, std::ostream& out)
{
  return run_rpc_loop(unlocked_session, open_context, provider, transport, transport, std::move(runtime_options), in, out, rpc::RpcInputWake{});
}

int run_rpc_mode(RpcModeOptions const& options, std::istream& in, std::ostream& out, std::ostream& err, rpc::RpcInputWake wake)
{
  ModeSignalLatch signal_latch;
  auto unlocked_session_result = runtime::Session::open(options.open_context, options.lifecycle_request);
  if (!unlocked_session_result)
  {
    err << unlocked_session_result.error().format() << '\n';
    return 1;
  }
  runtime::session_ts& unlocked_session = *unlocked_session_result;

  runtime::RunOptions runtime_options;
  runtime_options.cancel_requested = [&signal_latch] { return signal_latch.poll(); };
  // The Provider is kept alive till the end of this function.
  std::unique_ptr<ava::provider::Provider> provider;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);

    runtime_options.permission_resolver = build_headless_permission_resolver(options.permission_policy);
    runtime_options.question_resolver = nullptr;
    runtime_options.enable_transport_retries = true;
    runtime_options.offline = session_r->is_offline() || options.open_context.offline;

    auto catalog = options.open_context.provider_catalog ? options.open_context.provider_catalog : ava::provider::ProviderCatalog::build_builtins_only();
    auto provider_result = catalog->create(session_r->model().provider_id);
    if (!provider_result)
    {
      err << provider_result.error().format() << '\n';
      return 1;
    }
    provider = std::move(*provider_result);
  }

  auto const session_process_scope = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return session_r->session_process_scope();
  }();
  if (!session_process_scope)
  {
    err << "RPC process authority is unavailable\n";
    return 1;
  }
  ava::http::CurlCliTransport transport(*session_process_scope);
  ava::core::VoidResult result;
  if (&in == &std::cin)
  {
    auto input = rpc::make_posix_rpc_line_reader(STDIN_FILENO);
    if (!input)
    {
      err << input.error().format() << '\n';
      return 1;
    }
    result =
        run_rpc_loop_impl(unlocked_session, options.open_context, *provider, transport, transport, std::move(runtime_options), **input, out, &signal_latch);
  }
  else
  {
    result = run_rpc_loop(unlocked_session, options.open_context, *provider, transport, transport, std::move(runtime_options), in, out, std::move(wake));
  }
  if (signal_latch.signaled())
    return 130;
  if (!result)
  {
    err << result.error().format() << '\n';
    return 1;
  }
  return 0;
}

int run_rpc_mode(RpcModeOptions const& options, std::istream& in, std::ostream& out, std::ostream& err)
{
  return run_rpc_mode(options, in, out, err, rpc::RpcInputWake{});
}

}  // namespace ava::app

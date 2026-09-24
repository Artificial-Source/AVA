#include "sys.h"
#ifdef CWDEBUG
#include "ava/debug/debug_ostream_operators.h"
#endif
#include "ava/event/events.h"
#include "ava/http/curl_transport.h"
#include "ava/app/print_mode.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_credentials.h"
#include "ava/app/signal_policy.h"
#include "ava/tui/composer.h"
#include "ava/config/auth.h"
#include "ava/config/openai_oauth.h"
#include "ava/permissions/permission_rules.h"
#include "ava/provider/catalog.h"
#include "ava/provider/registry.h"
#include "ava/core/error.h"

#include <cerrno>
#include <concepts>
#include <iostream>
#include <iterator>
#include <memory>
#include <ostream>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <poll.h>
#include <unistd.h>

#ifdef CWDEBUG
#include <utils/at_scope_end.h>
#endif

namespace ava::app {
namespace {

bool has_value(std::optional<std::string> const& value)
{
  return value && !value->empty();
}

std::string read_all(std::istream& in)
{
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

ava::core::Result<std::string> read_signal_aware_stdin(ModeSignalLatch& signal_latch)
{
  std::string input;
  char buffer[8192];
  while (true)
  {
    if (signal_latch.poll())
      return input;
    pollfd descriptor{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    int const ready = ::poll(&descriptor, 1, 50);
    if (ready < 0)
    {
      if (errno == EINTR)
        continue;
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to poll print stdin"));
    }
    if (ready == 0)
      continue;
    ssize_t const bytes = ::read(STDIN_FILENO, buffer, sizeof(buffer));
    if (bytes > 0)
    {
      input.append(buffer, static_cast<std::size_t>(bytes));
      continue;
    }
    if (bytes == 0)
      return input;
    if (errno != EINTR)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to read print stdin"));
  }
}

ava::permissions::PermissionResolver deny_permission_resolver()
{
  return [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolutionDecision{
        ava::permissions::PermissionResolution::Deny,
        "print mode denied permission by default; use --allow read-only or --allow-tool <tool> for supported prompts"};
  };
}

std::string sanitize_terminal_output_text(std::string_view text)
{
  std::string output;
  output.reserve(text.size());
  std::size_t start = 0;
  for (std::size_t index = 0; index < text.size(); ++index)
  {
    if (text[index] != '\n')
      continue;
    output += ava::tui::sanitize_terminal_text(text.substr(start, index - start));
    output.push_back('\n');
    start = index + 1;
  }
  output += ava::tui::sanitize_terminal_text(text.substr(start));
  return output;
}

std::string terminal_output_text(std::string_view text, bool sanitize)
{
  return sanitize ? sanitize_terminal_output_text(text) : std::string(text);
}

void write_text_event_diagnostic(ava::event::RuntimeEvent const& event, std::ostream& err, bool sanitize)
{
  std::visit(
      [&](auto const& concrete) {
        using Event = std::remove_cvref_t<decltype(concrete)>;
        if constexpr (std::same_as<Event, ava::event::ToolStartEvent>)
        {
          auto const& payload = concrete.payload;
          err << "tool_start";
          if (!payload.tool.empty())
            err << " " << terminal_output_text(payload.tool, sanitize);
          if (!payload.text.empty())
            err << ": " << terminal_output_text(payload.text, sanitize);
          err << '\n';
        }
        else if constexpr (std::same_as<Event, ava::event::ToolResultEvent>)
        {
          auto const& payload = concrete.payload;
          err << "tool_result";
          if (!payload.tool.empty())
            err << " " << terminal_output_text(payload.tool, sanitize);
          if (!payload.status.empty())
            err << " " << terminal_output_text(payload.status, sanitize);
          if (!payload.text.empty())
            err << ": " << terminal_output_text(payload.text, sanitize);
          err << '\n';
          if ((payload.error_code == "permission_denied" || payload.error_category == "permission_denied") && !payload.error_details.empty())
            err << terminal_output_text(payload.error_details, sanitize) << '\n';
        }
        else if constexpr (std::same_as<Event, ava::event::ErrorEvent>)
        {
          auto const& payload = concrete.payload;
          err << terminal_output_text(payload.error_details.empty() ? payload.error_message : payload.error_details, sanitize) << '\n';
        }
        else
        {
          static_assert(std::same_as<Event, ava::event::SessionStartEvent> || std::same_as<Event, ava::event::UserMessageEvent> ||
                        std::same_as<Event, ava::event::AssistantMessageEvent> || std::same_as<Event, ava::event::MessageUpdateEvent> ||
                        std::same_as<Event, ava::event::MessageEndEvent> || std::same_as<Event, ava::event::ReasoningStartEvent> ||
                        std::same_as<Event, ava::event::ReasoningDeltaEvent> || std::same_as<Event, ava::event::ReasoningEndEvent> ||
                        std::same_as<Event, ava::event::ProviderEvent> || std::same_as<Event, ava::event::ToolProgressEvent> ||
                        std::same_as<Event, ava::event::CompactionStartEvent> || std::same_as<Event, ava::event::CompactionEndEvent> ||
                        std::same_as<Event, ava::event::RetryEvent> || std::same_as<Event, ava::event::RetryTickEvent> ||
                        std::same_as<Event, ava::event::CancellationEvent> || std::same_as<Event, ava::event::CompletionEvent>);
        }
      },
      event.payload());
}

runtime::RunOptions print_runtime_options(runtime::RunOptions options)
{
  if (!options.permission_resolver)
  {
    options.permission_resolver = deny_permission_resolver();
  }
  options.question_resolver = nullptr;
  return options;
}

ava::event::RuntimeEvent runtime_error_event(runtime::session_ts const& unlocked_session, ava::core::Error const& error)
{
  ava::event::ErrorPayload payload;
  payload.error_category = ava::core::to_string(error.category());
  payload.error_message = error.message();
  payload.error_details = error.format();
  return ava::event::RuntimeEvent{ava::event::RuntimeEventMetadata{
                                      .timestamp = ava::session::now_timestamp(),
                                      .session_id = runtime::session_ts::crat(unlocked_session)->store.session_id(),
                                  },
                                  ava::event::ErrorEvent{.payload = std::move(payload)}};
}

}  // namespace

ava::core::Result<std::string> merge_print_prompt(PrintPromptInputs const& inputs)
{
  bool const has_explicit = has_value(inputs.explicit_prompt);
  bool const has_stdin = has_value(inputs.stdin_prompt);
  if (has_explicit && has_stdin)
    return *inputs.explicit_prompt + "\n\n" + *inputs.stdin_prompt;
  if (has_explicit)
    return *inputs.explicit_prompt;
  if (has_stdin)
    return *inputs.stdin_prompt;
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "print mode requires a prompt argument or stdin"));
}

ava::core::Result<ava::agent::AgentLoopResult> run_print_prompt(runtime::session_ts& unlocked_session, std::string const& prompt,
                                                                ava::provider::Provider const& provider, ava::http::Transport& transport,
                                                                PrintModeRunOptions const& options, std::ostream& out, std::ostream& err)
{
  DoutEntering(dc::runtime, "run_print_prompt(prompt_bytes=" << prompt.size() << ")");
#ifdef CWDEBUG
  auto&& f = at_scope_end([] { Dout(dc::notice, "Leaving run_print_prompt()"); });
#endif
  AVA_ASSERT_SESSION_UNLOCKED(unlocked_session, "calling run_print_prompt");

  bool emitted_error = false;
  auto runtime_options = print_runtime_options(options.runtime_options);
  runtime_options.permission_resolver = ava::permissions::build_persistent_permission_rule_resolver(
      runtime::session_ts::rat(unlocked_session)->permission_rule_store(), std::move(runtime_options.permission_resolver));
  ava::event::EventBus event_bus;
  if (options.output_format == PrintOutputFormat::Json)
  {
    event_bus.subscribe([&out, &emitted_error](ava::event::EventEnvelope const& envelope) {
      if (envelope.name == ava::event::to_string(ava::event::RuntimeEventType::Error))
        emitted_error = true;
      out << ava::event::serialize_event_envelope_jsonl(envelope);
      if (!out)
      {
        return ava::core::VoidResult{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to write print JSON event"))};
      }
      return ava::core::VoidResult{};
    });
    runtime_options.event_sink = ava::event::make_runtime_event_bus_adapter(event_bus);
  }
  else
  {
    runtime_options.event_sink = [&err, &emitted_error, sanitize = options.sanitize_terminal_diagnostics](ava::event::RuntimeEvent const& event) {
      if (event.type() == ava::event::RuntimeEventType::Error)
        emitted_error = true;
      write_text_event_diagnostic(event, err, sanitize);
      if (!err)
      {
        return ava::core::VoidResult{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to write print diagnostic"))};
      }
      return ava::core::VoidResult{};
    };
  }

  auto result = run_prompt(unlocked_session, prompt, provider, transport, runtime_options);
  if (!result)
  {
    if (!emitted_error)
    {
      if (options.output_format == PrintOutputFormat::Json)
      {
        // Best-effort fallback: preserve the runtime/provider error that caused the failed turn.
        static_cast<void>(event_bus.publish(ava::event::to_event_envelope(runtime_error_event(unlocked_session, result.error()))));
      }
      else
      {
        err << terminal_output_text(result.error().format(), options.sanitize_terminal_diagnostics) << '\n';
      }
    }
    return std::unexpected(result.error());
  }

  if (options.output_format == PrintOutputFormat::Text)
  {
    out << terminal_output_text(result->final_text, options.sanitize_terminal_output);
    if (!out)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to write print output"));
    }
  }
  return result;
}

int run_print_mode(PrintModeOptions const& options, std::istream& in, std::ostream& out, std::ostream& err)
{
  ModeSignalLatch signal_latch;
  bool const sanitize_stdout = ::isatty(STDOUT_FILENO) == 1;
  bool const sanitize_stderr = ::isatty(STDERR_FILENO) == 1;
  std::optional<std::string> stdin_prompt;
  if (options.read_stdin)
  {
    if (&in == &std::cin)
    {
      auto read = read_signal_aware_stdin(signal_latch);
      if (!read)
      {
        err << terminal_output_text(read.error().format(), sanitize_stderr) << '\n';
        return 1;
      }
      stdin_prompt = std::move(*read);
    }
    else
      stdin_prompt = read_all(in);
    if (signal_latch.poll())
      return 130;
  }

  auto prompt = merge_print_prompt(PrintPromptInputs{.explicit_prompt = options.explicit_prompt, .stdin_prompt = std::move(stdin_prompt)});
  if (!prompt)
  {
    err << terminal_output_text(prompt.error().format(), sanitize_stderr) << '\n';
    return 2;
  }

  auto unlocked_session_result = runtime::Session::open(options.open_context, options.lifecycle_request);
  if (!unlocked_session_result)
  {
    err << terminal_output_text(unlocked_session_result.error().format(), sanitize_stderr) << '\n';
    return 1;
  }
  runtime::session_ts& unlocked_session(*unlocked_session_result);

  CRITICAL_AREA_BEGIN_W(session);

  std::unique_ptr<ava::http::Transport> default_transport;
  if (!options.transport_override)
  {
    if (!session_w->session_process_scope())
    {
      err << "print mode process authority is unavailable\n";
      return 1;
    }
    default_transport = std::make_unique<ava::http::CurlCliTransport>(*session_w->session_process_scope());
  }
  ava::http::Transport& transport = options.transport_override ? options.transport_override->get() : *default_transport;
  ava::http::Transport& auth_transport = options.transport_override ? options.transport_override->get() : *default_transport;
  auto catalog = session_w->provider_catalog()           ? session_w->provider_catalog()
                 : options.open_context.provider_catalog ? options.open_context.provider_catalog
                                                         : ava::provider::ProviderCatalog::build_builtins_only();
  auto default_provider = catalog->create(session_w->model().provider_id);
  if (!default_provider)
  {
    err << terminal_output_text(default_provider.error().format(), sanitize_stderr) << '\n';
    return 1;
  }
  ava::provider::Provider const& provider =
      options.provider_override ? options.provider_override->get() : static_cast<ava::provider::Provider const&>(**default_provider);
  runtime::RunOptions runtime_options;
  runtime_options.offline = session_w->is_offline() || options.open_context.offline;
  if (!runtime_options.offline)
  {
    auto prepared = prepare_runtime_credentials(session_w->paths(), session_w->model().provider_id, runtime_options, auth_transport, "print mode", catalog);
    if (!prepared)
    {
      if (prepared.error().message().find("requires auth for provider") != std::string::npos)
      {
        err << "print mode requires auth for provider `" << terminal_output_text(session_w->model().provider_id, sanitize_stderr)
            << "`. Configure a credential in " << terminal_output_text(session_w->paths().auth_file.string(), sanitize_stderr)
            << " or the provider API key environment variable\n";
      }
      else
      {
        err << terminal_output_text(prepared.error().format(), sanitize_stderr) << '\n';
      }
      return 1;
    }
    runtime_options = std::move(*prepared);
  }

  CRITICAL_AREA_END_W(session);

  runtime_options.enable_transport_retries = !options.transport_override.has_value();
  runtime_options.permission_resolver = build_headless_permission_resolver(options.permission_policy);
  runtime_options.cancel_requested = [&signal_latch] { return signal_latch.poll(); };

  PrintModeRunOptions const run_options{.output_format = options.output_format,
                                        .runtime_options = std::move(runtime_options),
                                        .sanitize_terminal_output = sanitize_stdout,
                                        .sanitize_terminal_diagnostics = sanitize_stderr};
  auto result = run_print_prompt(unlocked_session, *prompt, provider, transport, run_options, out, err);
  return signal_latch.signaled() ? 130 : (result ? 0 : 1);
}

}  // namespace ava::app

#include "sys.h"
#include "ava/http/curl_transport.h"
#include "ava/app/runtime/RunOptions.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_credentials.h"
#include "ava/app/runtime_model.h"
#include "ava/app/session_title_coordinator.h"
#include "ava/provider/catalog.h"
#include "ava/provider/registry.h"
#include "ava/core/json.h"
#include "ava/core/thread.h"
#include "ava/core/utf8.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

bool ascii_space(unsigned char ch)
{
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

std::size_t utf8_prefix_size(std::string_view text, std::size_t limit)
{
  std::size_t index = 0;
  std::size_t last = 0;
  while (index < text.size() && index < limit)
  {
    auto const decoded = ava::core::decode_utf8_scalar(text, index);
    if (!decoded || decoded->length > limit - index)
      break;
    index += decoded->length;
    last = index;
  }
  return last;
}

std::string lower_ascii(std::string_view text)
{
  std::string result;
  result.reserve(text.size());
  for (char ch : text)
  {
    auto const byte = static_cast<unsigned char>(ch);
    result.push_back(byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A')) : ch);
  }
  return result;
}

bool ascii_punctuation(unsigned char ch)
{
  return (ch >= '!' && ch <= '/') || (ch >= ':' && ch <= '@') || (ch >= '[' && ch <= '`') || (ch >= '{' && ch <= '~');
}

void trim_ascii_punctuation(std::string& word)
{
  while (!word.empty() && ascii_punctuation(static_cast<unsigned char>(word.front())))
    word.erase(word.begin());
  while (!word.empty() && ascii_punctuation(static_cast<unsigned char>(word.back())))
    word.pop_back();
}

void remove_tagged_section(std::string& text, std::string_view tag)
{
  auto lower = lower_ascii(text);
  auto const open_prefix = "<" + std::string(tag);
  auto const close = "</" + std::string(tag) + ">";
  std::size_t search = 0;
  while (true)
  {
    auto const start = lower.find(open_prefix, search);
    if (start == std::string::npos)
      break;
    auto const boundary = start + open_prefix.size();
    if (boundary >= lower.size() || (lower[boundary] != '>' && !ascii_space(static_cast<unsigned char>(lower[boundary]))))
    {
      search = boundary;
      continue;
    }
    auto const open_end = lower.find('>', boundary);
    if (open_end == std::string::npos)
    {
      text.erase(start);
      break;
    }
    auto const close_start = lower.find(close, open_end + 1);
    auto const end = close_start == std::string::npos ? text.size() : close_start + close.size();
    text.erase(start, end - start);
    lower.erase(start, end - start);
    search = start;
  }
}

std::string normalized_whitespace(std::string_view text)
{
  std::string result;
  result.reserve(text.size());
  bool pending_space = false;
  for (std::size_t index = 0; index < text.size();)
  {
    auto const byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x80U)
    {
      if (ascii_space(byte))
      {
        pending_space = !result.empty();
      }
      else if (byte >= 0x20U && byte != 0x7fU)
      {
        if (pending_space)
          result.push_back(' ');
        pending_space = false;
        result.push_back(static_cast<char>(byte));
      }
      else
      {
        pending_space = !result.empty();
      }
      ++index;
      continue;
    }
    auto const decoded = ava::core::decode_utf8_scalar(text, index);
    if (!decoded)
    {
      pending_space = !result.empty();
      ++index;
      continue;
    }
    if (pending_space)
      result.push_back(' ');
    pending_space = false;
    result.append(text.substr(index, decoded->length));
    index += decoded->length;
  }
  return result;
}

std::string trim_ascii(std::string_view text)
{
  std::size_t begin = 0;
  while (begin < text.size() && ascii_space(static_cast<unsigned char>(text[begin])))
    ++begin;
  std::size_t end = text.size();
  while (end > begin && ascii_space(static_cast<unsigned char>(text[end - 1])))
    --end;
  return std::string(text.substr(begin, end - begin));
}

bool contains_control(std::string_view text)
{
  return std::ranges::any_of(text, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return (byte < 0x20U && !ascii_space(byte)) || byte == 0x7fU;
  });
}

void strip_line_markup(std::string& line)
{
  auto trimmed = trim_ascii(line);
  while (!trimmed.empty() && (trimmed.front() == '#' || trimmed.front() == '>' || trimmed.front() == '-' || trimmed.front() == '*' || trimmed.front() == '`'))
  {
    trimmed.erase(trimmed.begin());
    trimmed = trim_ascii(trimmed);
  }
  while (!trimmed.empty() && trimmed.back() == '`')
    trimmed.pop_back();
  trimmed = trim_ascii(trimmed);
  if (trimmed.size() >= 2 && ((trimmed.front() == '"' && trimmed.back() == '"') || (trimmed.front() == '\'' && trimmed.back() == '\'') ||
                              (trimmed.front() == '`' && trimmed.back() == '`')))
  {
    trimmed = trim_ascii(std::string_view(trimmed).substr(1, trimmed.size() - 2));
  }
  auto lower = lower_ascii(trimmed);
  for (auto const prefix : {std::string_view("conversation title:"), std::string_view("title:")})
  {
    if (lower.starts_with(prefix))
    {
      trimmed = trim_ascii(std::string_view(trimmed).substr(prefix.size()));
      break;
    }
  }

  std::string without_markup;
  without_markup.reserve(trimmed.size());
  bool in_tag = false;
  for (std::size_t index = 0; index < trimmed.size(); ++index)
  {
    auto const ch = trimmed[index];
    if (ch == '<')
    {
      in_tag = true;
      continue;
    }
    if (in_tag)
    {
      if (ch == '>')
        in_tag = false;
      continue;
    }
    if (ch == '[')
    {
      auto const label_end = trimmed.find("](", index + 1);
      if (label_end != std::string::npos)
      {
        auto const link_end = trimmed.find(')', label_end + 2);
        if (link_end != std::string::npos)
        {
          without_markup.append(trimmed, index + 1, label_end - index - 1);
          index = link_end;
          continue;
        }
      }
    }
    without_markup.push_back(ch);
  }
  line = normalized_whitespace(without_markup);
}

void clear_secret(std::string& value) noexcept
{
  std::fill(value.begin(), value.end(), '\0');
  value.clear();
}

class DeadlineTransport final : public ava::http::Transport
{
 public:
  DeadlineTransport(std::unique_ptr<ava::http::Transport> inner, std::stop_token stop_token, std::chrono::steady_clock::time_point deadline)
      : inner_(std::move(inner)), stop_token_(stop_token), deadline_(deadline)
  {
  }

  ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override { return send(request, nullptr); }

  ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request, CancelCallback caller_cancel) override
  {
    auto bounded = request;
    auto const now = std::chrono::steady_clock::now();
    auto const remaining = now < deadline_ ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now).count() : 0;
    bounded.timeout_ms = static_cast<int>(std::clamp<long long>(remaining, 1, std::numeric_limits<int>::max()));
    return inner_->send(bounded, [this, caller_cancel] { return canceled() || (caller_cancel && caller_cancel()); });
  }

  bool supports_streaming() const noexcept override { return inner_->supports_streaming(); }

 private:
  bool canceled() const noexcept { return stop_token_.stop_requested() || std::chrono::steady_clock::now() >= deadline_; }

  std::unique_ptr<ava::http::Transport> inner_;
  std::stop_token stop_token_;
  std::chrono::steady_clock::time_point deadline_;
};

ava::core::Result<ava::config::ModelInfo> title_model(SessionTitleGenerationRequest const& request)
{
  if (!request.config.model_id)
    return request.active_model;
  auto const provider = request.config.provider_id.value_or(request.active_model.provider_id);
  auto model = resolve_runtime_model(request.paths, request.provider_catalog, provider, *request.config.model_id);
  if (!model)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "configured session title model is unavailable");
    error.with_context("provider", provider).with_context("model", *request.config.model_id);
    return std::unexpected(std::move(error));
  }
  return *model;
}

ava::core::Result<std::string> default_generate_title(SessionTitleGenerationRequest& generation, std::stop_token stop_token,
                                                      std::chrono::steady_clock::time_point deadline)
{
  if (generation.offline)
    return std::unexpected(offline_provider_error("session title generation"));
  if (!generation.anchor_set || !generation.anchor_set->contains_lexical(generation.paths.ava_config_dir) ||
      !generation.anchor_set->contains_lexical(generation.paths.ava_state_dir))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "session title storage authority is unavailable"));
  }
  auto model = title_model(generation);
  if (!model)
    return std::unexpected(std::move(model.error()));

  runtime::RunOptions credentials;
  credentials.access_token = generation.access_token;
  credentials.credential_type = generation.credential_type;
  credentials.openai_oauth = generation.openai_oauth;
  credentials.openai_account_id = generation.account_id;
  bool const cross_provider = model->provider_id != generation.active_model.provider_id;
  if (cross_provider)
  {
    clear_secret(credentials.access_token);
    credentials.credential_type = "bearer";
    credentials.openai_oauth = false;
    credentials.openai_account_id.clear();
  }
  credentials.offline = generation.offline;

  if (!generation.transport_factory)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title transport process authority is unavailable"));
  auto auth_inner = generation.transport_factory();
  if (!auth_inner || !*auth_inner)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title transport is unavailable"));
  DeadlineTransport auth_transport(std::move(*auth_inner), stop_token, deadline);
  auto prepared = prepare_runtime_credentials(generation.paths, model->provider_id, std::move(credentials), auth_transport, "session title generation",
                                              generation.provider_catalog);
  if (!prepared)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title credentials are unavailable"));

  auto catalog = ava::provider::ProviderCatalog::build_builtins_only();
  if (generation.provider_catalog)
    catalog = generation.provider_catalog;
  auto provider = catalog->create(model->provider_id);
  if (!provider)
  {
    clear_secret(prepared->access_token);
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title provider is unavailable"));
  }
  bool const stream = prepared->openai_oauth && model->supports_streaming.value_or(true);
  auto const max_tokens =
      model->max_output_tokens ? std::optional<long long>(std::min<long long>(*model->max_output_tokens, 64)) : std::optional<long long>(64);
  ava::provider::ProviderRequest const provider_request{
      .provider_id = model->provider_id,
      .model_id = model->model_id,
      .system_prompt = "Create one natural 5-10-word conversation title. Return only the title, with no reasoning, quotes, markup, or trailing punctuation.",
      .messages = {ava::provider::ChatMessage{.role = "user", .content = generation.source_text}},
      .tools_json = {},
      .stream = stream,
      .max_output_tokens = max_tokens,
      .compatibility_quirks = model->compatibility_quirks};
  ava::provider::ProviderAuthContext auth{
      .access_token = prepared->access_token,
      .credential_type = prepared->openai_oauth && prepared->credential_type == "bearer" ? "oauth" : prepared->credential_type,
      .account_id = prepared->openai_account_id};
  auto request = (*provider)->build_request(provider_request, auth);
  clear_secret(auth.access_token);
  clear_secret(prepared->access_token);
  if (!request)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title request could not be built"));

  auto transport_inner = generation.transport_factory();
  if (!transport_inner || !*transport_inner)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title transport is unavailable"));
  DeadlineTransport transport(std::move(*transport_inner), stop_token, deadline);
  auto response = transport.send(*request, [&] { return stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline; });
  for (auto& [name, value] : request->headers)
  {
    auto const lower_name = lower_ascii(name);
    if (lower_name == "authorization" || lower_name == "x-api-key" || lower_name == "x-goog-api-key")
      clear_secret(value);
  }
  if (!response)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title request failed"));
  if (response->body.size() > kMaxSessionTitleProviderOutputBytes)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title response exceeded the output limit"));
  auto events = (*provider)->parse_response(*response, stream);
  if (!events)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title response was malformed"));
  std::string output;
  for (auto const& event : *events)
  {
    if (event.type == ava::provider::StreamEventType::Error)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title provider returned an error"));
    if (event.type != ava::provider::StreamEventType::TextDelta)
      continue;
    if (event.text.size() > kMaxSessionTitleProviderOutputBytes - std::min(output.size(), kMaxSessionTitleProviderOutputBytes))
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title response exceeded the output limit"));
    output += event.text;
  }
  if (output.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "session title response contained no title"));
  return output;
}

bool metadata_is_root_title_candidate(ava::session::SessionMetadataView const& metadata)
{
  bool const root_origin = metadata.branch_origin.empty() || metadata.branch_origin == "root";
  return !metadata.has_manual_name && metadata.parent_session_id.empty() && metadata.source_session_id.empty() && root_origin;
}

bool metadata_allows_fallback(ava::session::SessionMetadataView const& metadata)
{
  return metadata_is_root_title_candidate(metadata) && metadata.generated_title.empty();
}

bool metadata_allows_refinement(ava::session::SessionMetadataView const& metadata, std::string_view fallback_title)
{
  return metadata_is_root_title_candidate(metadata) && metadata.generated_title == fallback_title;
}

bool captured_commit_belongs_to_first_ordinary_turn(std::vector<ava::session::SessionEntry> const& entries, std::string_view committed_turn_id)
{
  bool inside_first_ordinary_turn = false;
  bool found_first_ordinary_user = false;
  for (auto const& entry : entries)
  {
    if (entry.type == ava::session::EntryType::UserMessage)
    {
      auto const synthetic = ava::session::parse_synthetic_delivery_provenance(entry);
      bool const ordinary = synthetic && !*synthetic && !ava::session::is_internal_replay_user_message(entry);
      if (ordinary)
      {
        if (found_first_ordinary_user)
          inside_first_ordinary_turn = false;
        else
        {
          found_first_ordinary_user = true;
          inside_first_ordinary_turn = true;
        }
      }
    }
    if (entry.id == committed_turn_id)
      return inside_first_ordinary_turn && entry.type == ava::session::EntryType::AssistantTurnCommit;
  }
  return false;
}

enum class AutomaticTitleAppendOutcome
{
  Persisted,
  Ineligible,
  Failed,
};

template <typename MetadataPredicate>
AutomaticTitleAppendOutcome append_automatic_title(ava::session::SessionReadAuthority const& authority, ava::agent::SessionAppendSink const& append_route,
                                                   std::string_view committed_turn_id, std::string const& title, MetadataPredicate&& metadata_predicate)
{
  auto entries = authority.load();
  if (!entries || !captured_commit_belongs_to_first_ordinary_turn(*entries, committed_turn_id))
    return AutomaticTitleAppendOutcome::Ineligible;
  auto metadata = ava::session::session_metadata_from_entries({}, *entries);
  if (!metadata || !metadata_predicate(*metadata))
    return AutomaticTitleAppendOutcome::Ineligible;

  ava::session::SessionMetadataUpdate update;
  update.actor = "auto-title";
  update.generated_title = title;
  auto entry = ava::session::make_session_metadata_entry(std::move(update), entries->empty() ? std::string{} : entries->back().id);
  if (!entry || !append_route)
    return AutomaticTitleAppendOutcome::Failed;
  // Mutation errors are terminal at the controller-owned route. In particular,
  // even a proven not-started write is never retried by this best-effort caller;
  // the controller owns the stable append-state latch and recovery policy.
  return append_route(std::move(*entry)) ? AutomaticTitleAppendOutcome::Persisted : AutomaticTitleAppendOutcome::Failed;
}

}  // namespace

std::string normalize_session_title_source(std::string_view text)
{
  std::string stripped(text.substr(0, std::min(text.size(), kMaxSessionTitleSourceBytes * 2)));
  for (auto const tag : {"system-reminder", "available_skills", "skill", "developer-reminder", "session-history", "session-history-since", "project-memory"})
    remove_tagged_section(stripped, tag);
  auto normalized = normalized_whitespace(stripped);
  if (normalized.size() > kMaxSessionTitleSourceBytes)
    normalized.resize(utf8_prefix_size(normalized, kMaxSessionTitleSourceBytes));
  return normalized;
}

ava::core::Result<std::string> sanitize_generated_session_title(std::string_view text)
{
  if (text.size() > kMaxSessionTitleProviderOutputBytes || !ava::core::json::is_valid_utf8(text) || contains_control(text))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "generated session title is invalid"));
  std::string cleaned(text);
  remove_tagged_section(cleaned, "think");
  remove_tagged_section(cleaned, "reasoning");
  remove_tagged_section(cleaned, "analysis");
  std::size_t start = 0;
  while (start <= cleaned.size())
  {
    auto const end = cleaned.find_first_of("\r\n", start);
    auto line = std::string_view(cleaned).substr(start, end == std::string::npos ? cleaned.size() - start : end - start);
    auto candidate = trim_ascii(line);
    if (!candidate.empty() && candidate != "```" && !candidate.starts_with("```") && !candidate.starts_with("<think"))
    {
      strip_line_markup(candidate);
      if (!candidate.empty())
      {
        std::string bounded;
        std::size_t cursor = 0;
        std::size_t words = 0;
        while (cursor < candidate.size() && words < 10)
        {
          while (cursor < candidate.size() && candidate[cursor] == ' ')
            ++cursor;
          if (cursor >= candidate.size())
            break;
          auto end_word = candidate.find(' ', cursor);
          if (end_word == std::string::npos)
            end_word = candidate.size();
          auto word = candidate.substr(cursor, end_word - cursor);
          trim_ascii_punctuation(word);
          if (!word.empty())
          {
            constexpr std::size_t kMaxNaturalWordBytes = 40;
            if (word.size() > kMaxNaturalWordBytes)
              word.resize(utf8_prefix_size(word, kMaxNaturalWordBytes));
            auto const separator_bytes = bounded.empty() ? 0U : 1U;
            if (word.empty() || word.size() + separator_bytes > ava::session::kMaxGeneratedSessionTitleBytes - bounded.size())
              break;
            if (!bounded.empty())
              bounded.push_back(' ');
            bounded += word;
            ++words;
          }
          cursor = end_word;
        }
        if (words >= 5)
          return bounded;
      }
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
    if (start < cleaned.size() && cleaned[start - 1] == '\r' && cleaned[start] == '\n')
      ++start;
  }
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "generated session title is empty"));
}

std::string fallback_session_title(std::string_view normalized_source)
{
  auto source = normalized_whitespace(normalized_source);
  std::vector<std::string> title_words;
  std::size_t cursor = 0;
  while (cursor < source.size() && title_words.size() < 10)
  {
    while (cursor < source.size() && source[cursor] == ' ')
      ++cursor;
    auto end = source.find(' ', cursor);
    if (end == std::string::npos)
      end = source.size();
    auto word = std::string(source.substr(cursor, end - cursor));
    trim_ascii_punctuation(word);
    if (!word.empty())
    {
      constexpr std::size_t kMaxFallbackWordBytes = 28;
      if (word.size() > kMaxFallbackWordBytes)
        word.resize(utf8_prefix_size(word, kMaxFallbackWordBytes));
      if (!word.empty())
        title_words.push_back(std::move(word));
    }
    cursor = end;
  }
  if (title_words.empty())
    title_words = {"New", "Conversation"};
  constexpr std::array<std::string_view, 4> kFallbackWords = {"Overview", "and", "Next", "Steps"};
  std::size_t filler = 0;
  while (title_words.size() < 5)
    title_words.emplace_back(kFallbackWords[filler++]);

  std::string title;
  for (auto const& word : title_words)
  {
    auto const separator_bytes = title.empty() ? 0U : 1U;
    if (title.size() + separator_bytes + word.size() > ava::session::kMaxGeneratedSessionTitleBytes)
      break;
    if (!title.empty())
      title.push_back(' ');
    title += word;
  }
  return title;
}

SessionTitleCoordinator::SessionTitleCoordinator(SessionTitleCoordinatorOptions options) : options_(std::move(options))
{
  if (!options_.generator)
  {
    options_.generator = [](SessionTitleGenerationRequest& request, std::stop_token stop_token, std::chrono::steady_clock::time_point deadline) {
      return default_generate_title(request, stop_token, deadline);
    };
  }
}

ava::core::Result<std::shared_ptr<SessionTitleCoordinator>> SessionTitleCoordinator::create(SessionTitleCoordinatorOptions options)
{
  if (options.worker_count == 0 || options.worker_count > 4 || options.max_queued == 0 || options.max_queued > 128 || options.request_deadline.count() <= 0 ||
      options.request_deadline > std::chrono::seconds(30))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session title coordinator limits are invalid"));
  try
  {
    auto coordinator = std::shared_ptr<SessionTitleCoordinator>(new SessionTitleCoordinator(std::move(options)));
    coordinator->start();
    return coordinator;
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to start session title coordinator"));
  }
}

SessionTitleCoordinator::~SessionTitleCoordinator()
{
  shutdown();
}

void SessionTitleCoordinator::start()
{
  if (!options_.config.enabled)
    return;
  workers_.reserve(options_.worker_count);
  for (std::size_t index = 0; index < options_.worker_count; ++index)
    workers_.emplace_back(ava::core::JoinThread::create("session_title", [this](std::stop_token token) { worker_loop(token); }));
}

void SessionTitleCoordinator::schedule(runtime::session_ts const& unlocked_session, std::string_view original_user_text, std::string_view committed_turn_id,
                                       runtime::RunOptions const& run_options) noexcept
{
  if (!options_.config.enabled || committed_turn_id.empty())
    return;
  try
  {
    auto session_snapshot = [&]() {
      SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
      if (session_r->sessionless() || !session_r->created || !session_r->run_controller() || !session_r->anchor_set())
      {
        return std::optional<std::tuple<ava::session::SessionReadAuthority, ava::agent::SessionAppendSink, std::shared_ptr<SessionRunController>, std::string,
                                        ava::config::XdgPaths, ava::config::ModelInfo, std::shared_ptr<ava::core::AnchorSet>,
                                        std::shared_ptr<ava::provider::ProviderCatalog const>, bool, ava::http::TransportFactory>>{};
      }
      auto read_authority = session_r->read_authority_1();
      auto append_route = session_r->owner_append_route_1();
      if (!read_authority || !append_route)
      {
        return std::optional<std::tuple<ava::session::SessionReadAuthority, ava::agent::SessionAppendSink, std::shared_ptr<SessionRunController>, std::string,
                                        ava::config::XdgPaths, ava::config::ModelInfo, std::shared_ptr<ava::core::AnchorSet>,
                                        std::shared_ptr<ava::provider::ProviderCatalog const>, bool, ava::http::TransportFactory>>{};
      }
      auto transport_factory = options_.transport_factory;
      if (!transport_factory && session_r->session_process_scope())
      {
        transport_factory = [scope = *session_r->session_process_scope()]() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
          std::unique_ptr<ava::http::Transport> transport = std::make_unique<ava::http::CurlCliTransport>(scope);
          return transport;
        };
      }
      return std::optional{std::tuple{std::move(*read_authority), std::move(append_route), session_r->run_controller(),
                                      std::string(session_r->store.session_id()), session_r->paths(), session_r->model(), session_r->anchor_set(),
                                      session_r->provider_catalog(), session_r->is_offline(), std::move(transport_factory)}};
    }();
    if (!session_snapshot)
      return;
    auto [read_authority, append_route, run_controller, session_id, paths, active_model, anchor_set, provider_catalog, session_offline, transport_factory] =
        std::move(*session_snapshot);
    {
      std::lock_guard lock(mutex_);
      if (!admitted_session_ids_.insert(session_id).second)
        return;
    }

    auto const source_text = normalize_session_title_source(original_user_text);
    auto const fallback_title = fallback_session_title(source_text);
    auto const fallback = append_automatic_title(read_authority, append_route, committed_turn_id, fallback_title, metadata_allows_fallback);
    if (fallback != AutomaticTitleAppendOutcome::Persisted)
      return;
    publish_catalog_change(session_id);

    Work work{.session_id = session_id,
              .committed_turn_id = std::string(committed_turn_id),
              .fallback_title = fallback_title,
              .read_authority = std::move(read_authority),
              .append_controller = std::move(run_controller),
              .append_route = std::move(append_route),
              .request = SessionTitleGenerationRequest{.paths = std::move(paths),
                                                       .active_model = std::move(active_model),
                                                       .config = options_.config,
                                                       .anchor_set = std::move(anchor_set),
                                                       .provider_catalog = std::move(provider_catalog),
                                                       .source_text = source_text,
                                                       .access_token = run_options.access_token,
                                                       .credential_type = run_options.credential_type,
                                                       .account_id = run_options.openai_account_id,
                                                       .openai_oauth = run_options.openai_oauth,
                                                       .offline = session_offline || run_options.offline,
                                                       .transport_factory = std::move(transport_factory)}};
    std::lock_guard lock(mutex_);
    if (!accepting_ || queue_.size() >= options_.max_queued)
    {
      clear_secret(work.request.access_token);
      return;
    }
    active_session_ids_.insert(work.session_id);
    queue_.push_back(std::move(work));
    changed_.notify_one();
  }
  catch (...)
  {
  }
}

void SessionTitleCoordinator::worker_loop(std::stop_token stop_token)
{
  while (!stop_token.stop_requested())
  {
    std::optional<Work> work;
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, stop_token, [&] { return !queue_.empty() || !accepting_; });
      if (stop_token.stop_requested() || (!accepting_ && queue_.empty()))
        break;
      if (queue_.empty())
        continue;
      work.emplace(std::move(queue_.front()));
      queue_.pop_front();
    }
    auto const session_id = work->session_id;
    process(std::move(*work), stop_token);
    {
      std::lock_guard lock(mutex_);
      active_session_ids_.erase(session_id);
      changed_.notify_all();
    }
  }
}

void SessionTitleCoordinator::process(Work work, std::stop_token stop_token) noexcept
{
  auto finish = [&] { clear_secret(work.request.access_token); };
  try
  {
    auto entries = work.read_authority.load();
    if (!entries)
    {
      finish();
      return;
    }
    auto metadata = ava::session::session_metadata_from_entries({}, *entries);
    if (!metadata || !captured_commit_belongs_to_first_ordinary_turn(*entries, work.committed_turn_id) ||
        !metadata_allows_refinement(*metadata, work.fallback_title))
    {
      finish();
      return;
    }

    if (stop_token.stop_requested())
    {
      finish();
      return;
    }
    auto generated = options_.generator(work.request, stop_token, std::chrono::steady_clock::now() + options_.request_deadline);
    if (!generated || stop_token.stop_requested())
    {
      finish();
      return;
    }
    auto sanitized = sanitize_generated_session_title(*generated);
    if (!sanitized)
    {
      finish();
      return;
    }

    auto const outcome =
        append_automatic_title(work.read_authority, work.append_route, work.committed_turn_id, *sanitized,
                               [&](ava::session::SessionMetadataView const& candidate) { return metadata_allows_refinement(candidate, work.fallback_title); });
    if (outcome == AutomaticTitleAppendOutcome::Persisted)
      publish_catalog_change(work.session_id);
  }
  catch (...)
  {
  }
  finish();
}

void SessionTitleCoordinator::publish_catalog_change(std::string const& session_id)
{
  std::lock_guard lock(mutex_);
  auto const cursor = catalog_generation_.load(std::memory_order_relaxed) + 1;
  catalog_notifications_.push_back(CatalogNotification{.cursor = cursor, .session_id = session_id});
  catalog_generation_.store(cursor, std::memory_order_release);
}

SessionTitleCatalogChanges SessionTitleCoordinator::catalog_changes_since(std::size_t cursor) const
{
  std::lock_guard lock(mutex_);
  SessionTitleCatalogChanges changes;
  changes.cursor = catalog_generation_.load(std::memory_order_relaxed);
  std::unordered_set<std::string> seen;
  for (auto const& notification : catalog_notifications_)
  {
    if (notification.cursor > cursor && seen.insert(notification.session_id).second)
      changes.dirty_session_ids.push_back(notification.session_id);
  }
  return changes;
}

bool SessionTitleCoordinator::wait_until_idle(std::chrono::milliseconds timeout)
{
  std::unique_lock lock(mutex_);
  return changed_.wait_for(lock, timeout, [&] { return queue_.empty() && active_session_ids_.empty(); });
}

void SessionTitleCoordinator::shutdown() noexcept
{
  // This function potentially joins threads which is blocking operation.
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling SessionTitleCoordinator::shutdown()");

  std::vector<ava::core::JoinThread> workers;
  {
    std::lock_guard lock(mutex_);
    if (!accepting_ && workers_.empty())
      return;
    accepting_ = false;
    for (auto& worker : workers_)
      worker.request_stop();
    for (auto& work : queue_)
      clear_secret(work.request.access_token);
    queue_.clear();
    active_session_ids_.clear();
    workers.swap(workers_);
    changed_.notify_all();
  }
  workers.clear();
}

}  // namespace ava::app

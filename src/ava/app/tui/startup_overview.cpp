#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/terminal_text.h"
#include "ava/app/tui/startup_overview.h"
#include "ava/agent/mode.h"
#include "ava/tui/composer.h"
#include "ava/context/context_loader.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

[[nodiscard]] bool looks_like_path_or_secret(std::string_view text) noexcept
{
  if (text.empty())
    return false;
  if (text.find('/') != std::string_view::npos || text.find('\\') != std::string_view::npos)
    return true;
  if (text.starts_with("~") || text.starts_with("file:"))
    return true;
  auto lower = std::string(text);
  for (auto& ch : lower)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  static constexpr std::string_view kSecretMarkers[] = {"api_key", "apikey", "token", "secret", "password", "passwd", "authorization", "bearer "};
  for (auto const marker : kSecretMarkers)
  {
    if (lower.find(marker) != std::string::npos)
      return true;
  }
  return false;
}

[[nodiscard]] bool is_utf8_continuation_byte(unsigned char byte) noexcept
{
  return (byte & 0xC0U) == 0x80U;
}

// Longest valid UTF-8 prefix that fits in max_bytes. Never splits a multibyte sequence.
[[nodiscard]] std::string_view utf8_prefix_within(std::string_view value, std::size_t max_bytes) noexcept
{
  if (value.size() <= max_bytes)
    return value;
  if (max_bytes == 0)
    return {};

  std::size_t index = 0;
  std::size_t end = 0;
  while (index < value.size() && index < max_bytes)
  {
    auto const byte = static_cast<unsigned char>(value[index]);
    std::size_t length = 0;
    if (byte < 0x80U)
      length = 1;
    else if (byte >= 0xC2U && byte <= 0xDFU)
      length = 2;
    else if ((byte & 0xF0U) == 0xE0U)
      length = 3;
    else if (byte >= 0xF0U && byte <= 0xF4U)
      length = 4;
    else
    {
      // Invalid lead: treat as a single bounded byte so work stays proportional.
      length = 1;
    }
    if (length == 0 || index + length > value.size())
      break;
    if (length > 1)
    {
      bool valid = true;
      for (std::size_t offset = 1; offset < length; ++offset)
      {
        if (!is_utf8_continuation_byte(static_cast<unsigned char>(value[index + offset])))
        {
          valid = false;
          break;
        }
      }
      if (!valid)
        length = 1;
    }
    if (index + length > max_bytes)
      break;
    index += length;
    end = index;
  }
  return value.substr(0, end);
}

// Bound raw bytes first (UTF-8 safe), sanitize the bounded prefix only, then
// ellipsize within max_bytes without splitting a multibyte sequence.
[[nodiscard]] std::string sanitize_overview_label(std::string_view raw, std::size_t max_bytes)
{
  if (max_bytes == 0)
    return {};

  auto const raw_truncated = raw.size() > max_bytes;
  auto const bounded_raw = utf8_prefix_within(raw, max_bytes);
  auto text = ava::app::sanitize_terminal_text(bounded_raw);
  // Drop path-like or secret-looking labels rather than leak private data.
  if (looks_like_path_or_secret(text))
    return {};

  if (text.size() <= max_bytes && !raw_truncated)
    return text;

  if (max_bytes <= 3)
    return std::string(max_bytes, '.');

  auto const body_budget = max_bytes - 3;
  if (text.size() > body_budget)
    text = std::string(utf8_prefix_within(text, body_budget));
  text += "...";
  return text;
}

void push_unique_sorted(std::vector<std::string>& values, std::string value, std::size_t max_items, bool& truncated)
{
  if (value.empty())
    return;
  if (std::ranges::find(values, value) != values.end())
    return;
  if (values.size() >= max_items)
  {
    truncated = true;
    return;
  }
  values.push_back(std::move(value));
  std::ranges::sort(values);
}

[[nodiscard]] std::string_view freshness_kind_label(runtime::FreshnessSourceKind kind) noexcept
{
  switch (kind)
  {
    case runtime::FreshnessSourceKind::SystemPrompt:
      return "system_prompt";
    case runtime::FreshnessSourceKind::AppendSystemPrompt:
      return "append_system_prompt";
    case runtime::FreshnessSourceKind::PromptCommand:
      return "prompt_command";
    case runtime::FreshnessSourceKind::Skill:
      return "skill";
    case runtime::FreshnessSourceKind::PluginManifest:
      return "plugin";
    case runtime::FreshnessSourceKind::PluginPrompt:
      return "plugin_prompt";
    case runtime::FreshnessSourceKind::PluginSkill:
      return "plugin_skill";
  }
  return "resource";
}

[[nodiscard]] std::string_view context_kind_label(ava::context::ContextSourceType type) noexcept
{
  switch (type)
  {
    case ava::context::ContextSourceType::Workspace:
      return "project";
    case ava::context::ContextSourceType::Global:
      return "global";
    case ava::context::ContextSourceType::Plugin:
      return "plugin";
  }
  return "instruction";
}

[[nodiscard]] std::string first_key_display(ava::tui::TuiKeyBindings const& bindings, ava::tui::TuiAction action)
{
  for (auto const& [configured, keys] : bindings.bindings)
  {
    if (configured != action || keys.empty())
      continue;
    auto display = ava::tui::key_display(keys.front());
    if (!display.empty())
      return display;
  }
  return {};
}

void append_compact_segment(std::string& line, std::string_view segment, std::size_t max_bytes)
{
  if (segment.empty())
    return;
  auto candidate = line;
  if (!candidate.empty())
    candidate += " · ";
  candidate += segment;
  if (candidate.size() > max_bytes)
    return;
  line = std::move(candidate);
}

[[nodiscard]] ava::tui::StartupOverviewResourceGroup make_group(std::string kind, std::string scope, std::size_t count, std::vector<std::string> labels,
                                                                bool count_is_lower_bound = false)
{
  if (labels.size() > kMaxStartupOverviewLabelsPerGroup)
    labels.resize(kMaxStartupOverviewLabelsPerGroup);
  return ava::tui::StartupOverviewResourceGroup{
      .kind = std::move(kind), .scope = std::move(scope), .count = count, .count_is_lower_bound = count_is_lower_bound, .labels = std::move(labels)};
}

[[nodiscard]] std::string bounded_count_text(std::size_t count, bool is_lower_bound)
{
  auto text = std::to_string(count);
  if (is_lower_bound)
    text += '+';
  return text;
}

// Failed plugin prompt/skill freshness rows are retained with empty load snapshots
// (byte_count == 0 && content_fingerprint == 0) by add_failed_plugin_resource_freshness_source.
// Successful empty content still receives a non-zero FNV fingerprint, so the dual-zero
// contract is stable and not an ambiguous zero-hash inference.
[[nodiscard]] bool is_failed_plugin_resource_snapshot(runtime::FreshnessSourceMetadata const& source) noexcept
{
  return (source.kind == runtime::FreshnessSourceKind::PluginPrompt || source.kind == runtime::FreshnessSourceKind::PluginSkill) && source.byte_count == 0 &&
         source.content_fingerprint == 0;
}

[[nodiscard]] std::string count_label(std::size_t count, bool truncated, std::string_view unit)
{
  auto label = std::to_string(count);
  if (truncated)
    label += '+';
  label += ' ';
  label += unit;
  return label;
}

}  // namespace

std::string startup_overview_toggle_keys_display(ava::tui::TuiKeyBindings const& bindings)
{
  return first_key_display(bindings, ava::tui::TuiAction::OverviewToggle);
}

ava::tui::StartupOverviewSnapshot build_startup_overview_snapshot(StartupOverviewBuildInput const& input)
{
  static auto const kDefaultBindings = ava::tui::default_key_bindings();
  auto const& bindings = input.key_bindings ? *input.key_bindings : kDefaultBindings;

  ava::tui::StartupOverviewSnapshot snapshot{
      .mode = sanitize_overview_label(input.mode, kMaxStartupOverviewLabelBytes),
      .provider = sanitize_overview_label(input.provider, kMaxStartupOverviewLabelBytes),
      .model = sanitize_overview_label(input.model, kMaxStartupOverviewLabelBytes),
      .trust_decision = sanitize_overview_label(input.trust_decision, kMaxStartupOverviewLabelBytes),
      .project_resources = sanitize_overview_label(input.project_resources, kMaxStartupOverviewLabelBytes),
      .protected_resource_count = input.protected_resource_count,
      .theme_name = sanitize_overview_label(input.theme_name, kMaxStartupOverviewLabelBytes),
      .theme_badge = sanitize_overview_label(input.theme_badge, kMaxStartupOverviewLabelBytes),
      .overview_toggle_keys = startup_overview_toggle_keys_display(bindings),
  };

  // Instruction sources: counts by kind/scope only. Never emit path leaves.
  // Bound input work at the first-N cap. The O(1) span size remains the exact total;
  // per-group counts from the capped prefix are conservative lower bounds (N+) when
  // the input exceeded the cap. Do not scan past the cap to classify omitted kinds.
  struct InstructionBucket
  {
    std::string kind;
    std::string scope;
    std::size_t count = 0;
  };
  std::vector<InstructionBucket> instruction_buckets;
  auto const context_total = input.context_sources.size();
  auto const context_limit = std::min(context_total, kMaxStartupOverviewInputSources);
  bool const context_truncated = context_total > context_limit;
  for (std::size_t index = 0; index < context_limit; ++index)
  {
    auto const& source = input.context_sources[index];
    auto const kind = std::string(context_kind_label(source.source_type));
    // Scope uses the stable context source-type string, not a path leaf.
    auto scope = sanitize_overview_label(ava::context::to_string(source.source_type), kMaxStartupOverviewLabelBytes);
    auto existing = std::ranges::find_if(instruction_buckets, [&](auto const& bucket) { return bucket.kind == kind && bucket.scope == scope; });
    if (existing == instruction_buckets.end())
      instruction_buckets.push_back(InstructionBucket{.kind = kind, .scope = std::move(scope), .count = 1});
    else
      ++existing->count;
  }
  std::ranges::sort(instruction_buckets, [](auto const& left, auto const& right) {
    if (left.kind != right.kind)
      return left.kind < right.kind;
    return left.scope < right.scope;
  });
  // Exact total via O(1) span size. Group rows mark lower bounds when work was capped.
  snapshot.instruction_source_count = context_total;
  for (auto const& bucket : instruction_buckets)
  {
    if (snapshot.resource_groups.size() >= kMaxStartupOverviewResourceGroups)
      break;
    snapshot.resource_groups.push_back(make_group("instruction", bucket.scope, bucket.count, {}, context_truncated));
  }

  // Freshness: group by kind+scope; prefer kind/scope labels over filename leaves.
  struct FreshnessBucket
  {
    runtime::FreshnessSourceKind kind = runtime::FreshnessSourceKind::Skill;
    std::string scope;
    std::size_t count = 0;
    std::vector<std::string> labels;
  };
  std::vector<FreshnessBucket> freshness_buckets;
  std::size_t plugin_resource_failures = 0;
  bool saw_plugin_resource = false;
  bool skill_names_truncated = false;
  bool prompt_names_truncated = false;
  bool plugin_ids_truncated = false;

  auto const freshness_total = input.freshness_sources.size();
  auto const freshness_limit = std::min(freshness_total, kMaxStartupOverviewInputSources);
  bool const freshness_truncated = freshness_total > freshness_limit;
  for (std::size_t index = 0; index < freshness_limit; ++index)
  {
    auto const& source = input.freshness_sources[index];
    auto scope = sanitize_overview_label(source.scope, kMaxStartupOverviewLabelBytes);
    if (scope.empty())
      scope = "unknown";

    auto existing = std::ranges::find_if(freshness_buckets, [&](auto const& bucket) { return bucket.kind == source.kind && bucket.scope == scope; });
    if (existing == freshness_buckets.end())
    {
      freshness_buckets.push_back(FreshnessBucket{.kind = source.kind, .scope = scope, .count = 0, .labels = {}});
      existing = freshness_buckets.end() - 1;
    }
    ++existing->count;

    std::string label;
    switch (source.kind)
    {
      case runtime::FreshnessSourceKind::SystemPrompt:
      case runtime::FreshnessSourceKind::AppendSystemPrompt:
        // Prefer kind/scope over SYSTEM.md-style filename leaves.
        label = sanitize_overview_label(std::string(freshness_kind_label(source.kind)) + " · " + scope, kMaxStartupOverviewLabelBytes);
        break;
      case runtime::FreshnessSourceKind::PromptCommand:
        label = sanitize_overview_label(source.name.empty() ? source.source_id : source.name, kMaxStartupOverviewLabelBytes);
        push_unique_sorted(snapshot.prompt_command_names, label, kMaxStartupOverviewNamedItems, prompt_names_truncated);
        break;
      case runtime::FreshnessSourceKind::Skill:
        label = sanitize_overview_label(source.name.empty() ? source.source_id : source.name, kMaxStartupOverviewLabelBytes);
        push_unique_sorted(snapshot.skill_names, label, kMaxStartupOverviewNamedItems, skill_names_truncated);
        break;
      case runtime::FreshnessSourceKind::PluginManifest:
        label = sanitize_overview_label(source.source_id.empty() ? source.name : source.source_id, kMaxStartupOverviewLabelBytes);
        push_unique_sorted(snapshot.plugin_ids, label, kMaxStartupOverviewNamedItems, plugin_ids_truncated);
        break;
      case runtime::FreshnessSourceKind::PluginPrompt:
      case runtime::FreshnessSourceKind::PluginSkill: {
        saw_plugin_resource = true;
        auto const plugin = sanitize_overview_label(source.source_id, kMaxStartupOverviewLabelBytes);
        auto const resource = sanitize_overview_label(source.name, kMaxStartupOverviewLabelBytes);
        if (!plugin.empty() && !resource.empty())
          label = sanitize_overview_label(plugin + " · " + resource, kMaxStartupOverviewLabelBytes);
        else
          label = plugin.empty() ? resource : plugin;
        if (is_failed_plugin_resource_snapshot(source))
          ++plugin_resource_failures;
        break;
      }
    }
    if (!label.empty())
    {
      bool labels_truncated = false;
      push_unique_sorted(existing->labels, std::move(label), kMaxStartupOverviewLabelsPerGroup, labels_truncated);
      static_cast<void>(labels_truncated);
    }
  }

  std::ranges::sort(freshness_buckets, [](auto const& left, auto const& right) {
    auto const left_kind = std::string(freshness_kind_label(left.kind));
    auto const right_kind = std::string(freshness_kind_label(right.kind));
    if (left_kind != right_kind)
      return left_kind < right_kind;
    return left.scope < right.scope;
  });
  for (auto& bucket : freshness_buckets)
  {
    if (snapshot.resource_groups.size() >= kMaxStartupOverviewResourceGroups)
      break;
    // Conservative: when freshness input exceeded the cap, every observed freshness
    // group count is a lower bound (omitted kinds past the cap are not scanned).
    snapshot.resource_groups.push_back(
        make_group(std::string(freshness_kind_label(bucket.kind)), std::move(bucket.scope), bucket.count, std::move(bucket.labels), freshness_truncated));
  }
  if (saw_plugin_resource)
  {
    snapshot.plugin_resource_failure_count = plugin_resource_failures;
    // Failures past the cap are invisible; surface N+ (including truthful 0+) when shown.
    snapshot.plugin_resource_failure_count_is_lower_bound = freshness_truncated;
  }

  // Small essential key hints derived from effective bindings.
  struct HintSpec
  {
    ava::tui::TuiAction action;
    std::string_view label;
  };
  static constexpr HintSpec kHints[] = {
      {ava::tui::TuiAction::OverviewToggle, "overview"}, {ava::tui::TuiAction::ModelSelect, "models"}, {ava::tui::TuiAction::ModeToggle, "mode"},
      {ava::tui::TuiAction::Submit, "submit"},           {ava::tui::TuiAction::Cancel, "cancel"},      {ava::tui::TuiAction::DetailsToggle, "details"},
  };
  for (auto const& hint : kHints)
  {
    if (snapshot.key_hints.size() >= kMaxStartupOverviewKeyHints)
      break;
    auto keys = first_key_display(bindings, hint.action);
    if (hint.action == ava::tui::TuiAction::OverviewToggle)
    {
      // Always surface the slash command; append a configured key when bound.
      snapshot.key_hints.push_back(ava::tui::StartupOverviewKeyHint{.label = std::string(hint.label), .keys = keys.empty() ? std::string("/overview") : keys});
      continue;
    }
    if (keys.empty())
      continue;
    snapshot.key_hints.push_back(ava::tui::StartupOverviewKeyHint{.label = std::string(hint.label), .keys = std::move(keys)});
  }

  // Compact collapsed chrome lines: counts and safe labels only — no private leaves.
  std::string compact;
  if (!snapshot.mode.empty())
    append_compact_segment(compact, snapshot.mode, kMaxStartupOverviewCompactBytes);
  if (!snapshot.provider.empty() || !snapshot.model.empty())
  {
    std::string model_bit = snapshot.provider;
    if (!snapshot.provider.empty() && !snapshot.model.empty())
      model_bit += "/";
    model_bit += snapshot.model;
    append_compact_segment(compact, model_bit, kMaxStartupOverviewCompactBytes);
  }
  if (!snapshot.trust_decision.empty())
    append_compact_segment(compact, std::string("trust ") + snapshot.trust_decision, kMaxStartupOverviewCompactBytes);
  // Instruction total is exact (O(1) span size). Named-list counts keep their own
  // truncation markers when the list or freshness work prefix was capped.
  if (snapshot.instruction_source_count > 0)
    append_compact_segment(compact, count_label(snapshot.instruction_source_count, /*truncated=*/false, "ctx"), kMaxStartupOverviewCompactBytes);
  if (!snapshot.skill_names.empty())
    append_compact_segment(compact, count_label(snapshot.skill_names.size(), skill_names_truncated || freshness_truncated, "skills"),
                           kMaxStartupOverviewCompactBytes);
  if (!snapshot.plugin_ids.empty())
    append_compact_segment(compact, count_label(snapshot.plugin_ids.size(), plugin_ids_truncated || freshness_truncated, "plugins"),
                           kMaxStartupOverviewCompactBytes);
  if (!snapshot.theme_name.empty())
    append_compact_segment(compact, snapshot.theme_name, kMaxStartupOverviewCompactBytes);

  std::string open_hint = "/overview";
  if (!snapshot.overview_toggle_keys.empty())
    open_hint = snapshot.overview_toggle_keys + " · /overview";
  append_compact_segment(compact, open_hint, kMaxStartupOverviewCompactBytes);
  snapshot.compact_line = std::move(compact);

  std::string detail;
  if (!snapshot.prompt_command_names.empty())
    append_compact_segment(detail, count_label(snapshot.prompt_command_names.size(), prompt_names_truncated || freshness_truncated, "cmds"),
                           kMaxStartupOverviewDetailBytes);
  if (snapshot.plugin_resource_failure_count && (*snapshot.plugin_resource_failure_count > 0 || snapshot.plugin_resource_failure_count_is_lower_bound))
  {
    append_compact_segment(detail,
                           bounded_count_text(*snapshot.plugin_resource_failure_count, snapshot.plugin_resource_failure_count_is_lower_bound) + " plugin fails",
                           kMaxStartupOverviewDetailBytes);
  }
  if (detail.empty() && snapshot.protected_resource_count > 0)
    append_compact_segment(detail, std::to_string(snapshot.protected_resource_count) + " protected", kMaxStartupOverviewDetailBytes);
  if (detail.empty() && !snapshot.project_resources.empty())
    append_compact_segment(detail, std::string("project ") + snapshot.project_resources, kMaxStartupOverviewDetailBytes);
  snapshot.detail_line = std::move(detail);

  return snapshot;
}

ava::tui::StartupOverviewSnapshot build_startup_overview_snapshot(runtime::session_ts const& session, ava::tui::TuiKeyBindings const& key_bindings,
                                                                  ava::tui::TuiThemeInfo const& theme)
{
  // Keep the non-owning spans guarded only for the builder's bounded, in-memory
  // projection; the returned snapshot owns everything it retains.
  SCOPED_CRITICAL_AREA_CR(session_r, session);
  auto const& trust = session_r->project_trust();
  auto const mode = ava::agent::to_string(session_r->mode());
  auto const& provider = session_r->model().provider_id;
  auto const& model = session_r->model().display_name.empty() ? session_r->model().model_id : session_r->model().display_name;
  auto const trust_decision = std::string(to_string(trust.decision));
  auto const project_resources = project_resources_trusted(trust) ? std::string("enabled") : std::string("skipped");
  StartupOverviewBuildInput input{
      .mode = mode,
      .provider = provider,
      .model = model,
      .trust_decision = trust_decision,
      .project_resources = project_resources,
      .protected_resource_count = trust.protected_resources.size(),
      .context_sources = session_r->context_sources(),
      .freshness_sources = session_r->freshness_sources(),
      .theme_name = theme.name,
      .theme_badge = theme.badge,
      .key_bindings = &key_bindings,
  };
  return build_startup_overview_snapshot(input);
}

}  // namespace ava::app

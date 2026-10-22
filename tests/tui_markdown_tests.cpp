#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/composer.h"
#include "ava/tui/text.h"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using tui_test_support::ScopedTerminalCapabilityProfile;

void run_tui_markdown_tests()
{
  tui_test_support::TuiFrontEndFixture frontend;
  // Markdown layout assertions do not exercise backend statistics; retain the
  // fixture's historical footer width independently from initial publication.
  frontend.set_active_token_estimates(std::nullopt, std::nullopt);
  auto const markdown_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{
                                     .label = "ava",
                                     .text = "First paragraph wraps cleanly across words.\n\nSecond paragraph stays separate.\n- bullet item\n* star "
                                             "item\n1. numbered item\n> quoted text\n```cpp\nint main() {}\n```\nUse `ava build` and "
                                             "**bold text**."}},
                                 .width = 72,
                                 .height = 24});
  expect(std::ranges::any_of(markdown_transcript,
                             [](std::string const& line) { return strip_sgr(line).find("First paragraph wraps cleanly") != std::string::npos; }) &&
             std::ranges::any_of(markdown_transcript,
                                 [](std::string const& line) { return strip_sgr(line).find("Second paragraph stays separate") != std::string::npos; }) &&
             std::ranges::any_of(markdown_transcript, [](std::string const& line) { return strip_sgr(line).find("- bullet item") != std::string::npos; }) &&
             std::ranges::any_of(markdown_transcript, [](std::string const& line) { return strip_sgr(line).find("* star item") != std::string::npos; }) &&
             std::ranges::any_of(markdown_transcript, [](std::string const& line) { return strip_sgr(line).find("1. numbered item") != std::string::npos; }) &&
             std::ranges::any_of(markdown_transcript, [](std::string const& line) { return strip_sgr(line).find("│ quoted text") != std::string::npos; }) &&
             std::ranges::any_of(markdown_transcript, [](std::string const& line) { return strip_sgr(line).find("``` cpp") != std::string::npos; }) &&
             std::ranges::any_of(markdown_transcript, [](std::string const& line) { return strip_sgr(line).find("  int main() {}") != std::string::npos; }) &&
             std::ranges::any_of(markdown_transcript,
                                 [](std::string const& line) { return strip_sgr(line).find("Use ava build and bold text") != std::string::npos; }) &&
             std::ranges::none_of(markdown_transcript,
                                  [](std::string const& line) {
                                    auto const visible = strip_sgr(line);
                                    return visible.find("`ava build`") != std::string::npos || visible.find("**bold text**") != std::string::npos;
                                  }),
         "tui assistant renderer handles paragraphs, lists, quotes, fenced code, inline code, and bold");

  constexpr auto kBoldSgr = std::string_view{"\x1b[1m"};
  constexpr auto kItalicSgr = std::string_view{"\x1b[3m"};
  constexpr auto kUnderlineSgr = std::string_view{"\x1b[4m"};
  constexpr auto kStrikethroughSgr = std::string_view{"\x1b[9m"};
  constexpr auto kResetSgr = std::string_view{"\x1b[0m"};
  constexpr auto kMutedSgr = std::string_view{"\x1b[38;2;139;149;165m"};
  constexpr auto kWarningSgr = std::string_view{"\x1b[38;2;251;191;36m"};
  constexpr auto kSuccessSgr = std::string_view{"\x1b[38;2;52;211;153m"};
  constexpr auto kErrorSgr = std::string_view{"\x1b[38;2;248;113;113m"};
  constexpr auto kAccentSgr = std::string_view{"\x1b[38;2;77;158;246m"};

  auto const role_markup_transcript = ava::tui::render_composer(
      frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                      //.provider = "openai",
                                                      //.model = "gpt-5.5",
                                                      .session_id = "session_test",
                                                      .input = "",
                                                      .status = "ready",
                                                      .transcript = {ava::tui::TranscriptItem{.label = "you", .text = "Use `x` and **y**."},
                                                                     ava::tui::TranscriptItem{.label = "ava", .text = "Use `x` and **y**."}},
                                                      .width = 28,
                                                      .height = 16});
  auto const user_markup_line =
      std::ranges::find_if(role_markup_transcript, [](std::string const& line) { return strip_sgr(line).find("Use `x` and **y**.") != std::string::npos; });
  auto const assistant_markup_line =
      std::ranges::find_if(role_markup_transcript, [](std::string const& line) { return strip_sgr(line).find("Use x and y.") != std::string::npos; });
  expect(user_markup_line != role_markup_transcript.end() && assistant_markup_line != role_markup_transcript.end() &&
             !has_active_sgr_at_text(*user_markup_line, "x", kWarningSgr) && has_active_sgr_at_text(*user_markup_line, "x", kBoldSgr) &&
             user_markup_line->find("\x1b[48;2;26;31;46m") == std::string::npos && visible_columns(*user_markup_line) <= 28 &&
             has_active_sgr_at_text(*assistant_markup_line, "x", kWarningSgr) && has_active_sgr_at_text(*assistant_markup_line, "y", kBoldSgr),
         "tui keeps user inline markdown literal in bright chevron-accented text while formatting assistant inline markdown");

  {
    ScopedTerminalCapabilityProfile no_hyperlink_terminal("");
    auto const inline_style_transcript = ava::tui::render_composer(
        frontend.frontend(),
        ava::tui::ComposerSnapshot{.mode = "build",
                                   //.provider = "openai",
                                   //.model = "gpt-5.5",
                                   .session_id = "session_test",
                                   .input = "",
                                   .status = "ready",
                                   .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "Use *italic* and [docs](https://example.test)."}},
                                   .width = 72,
                                   .height = 12});
    auto const inline_style_line = std::ranges::find_if(inline_style_transcript, [](std::string const& line) {
      return strip_sgr(line).find("Use italic and docs (https://example.test).") != std::string::npos;
    });
    expect(inline_style_line != inline_style_transcript.end() && has_active_sgr_at_text(*inline_style_line, "italic", kItalicSgr) &&
               has_active_sgr_at_text(*inline_style_line, "docs", kUnderlineSgr),
           "tui assistant renderer emits visible italic and link underline SGR for parsed Markdown inline styling");

    auto const heading_inline_transcript = ava::tui::render_composer(
        frontend.frontend(),
        ava::tui::ComposerSnapshot{.mode = "build",
                                   //.provider = "openai",
                                   //.model = "gpt-5.5",
                                   .session_id = "session_test",
                                   .input = "",
                                   .status = "ready",
                                   .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "### Why `sourceInfo` should stay **visible**."}},
                                   .width = 72,
                                   .height = 12});
    auto const heading_inline_line = std::ranges::find_if(
        heading_inline_transcript, [](std::string const& line) { return strip_sgr(line).find("Why sourceInfo should stay visible.") != std::string::npos; });
    expect(heading_inline_line != heading_inline_transcript.end() && strip_sgr(*heading_inline_line).find("###") == std::string::npos &&
               has_active_sgr_at_text(*heading_inline_line, "sourceInfo", kWarningSgr) &&
               has_active_sgr_at_text(*heading_inline_line, "should stay", kBoldSgr) && has_active_sgr_at_text(*heading_inline_line, "visible", kBoldSgr) &&
               strip_sgr(*heading_inline_line).find("`sourceInfo`") == std::string::npos &&
               strip_sgr(*heading_inline_line).find("**visible**") == std::string::npos,
           "tui assistant renderer preserves heading styling around inline code and bold Markdown spans");

    auto const strikethrough_transcript = ava::tui::render_composer(
        frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                        //.provider = "openai",
                                                        //.model = "gpt-5.5",
                                                        .session_id = "session_test",
                                                        .input = "",
                                                        .status = "ready",
                                                        .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "Use ~~strike~~ and ~literal~."}},
                                                        .width = 56,
                                                        .height = 12});
    auto const strikethrough_line = std::ranges::find_if(
        strikethrough_transcript, [](std::string const& line) { return strip_sgr(line).find("Use strike and ~literal~.") != std::string::npos; });
    expect(strikethrough_line != strikethrough_transcript.end() && has_active_sgr_at_text(*strikethrough_line, "strike", kStrikethroughSgr) &&
               !has_active_sgr_at_text(*strikethrough_line, "literal", kStrikethroughSgr) &&
               strip_sgr(*strikethrough_line).find("~~strike~~") == std::string::npos,
           "tui assistant renderer emits Pi-style strikethrough SGR for double-tilde Markdown while preserving single-tilde text");

    auto const link_fallback_transcript = ava::tui::render_composer(
        frontend.frontend(),
        ava::tui::ComposerSnapshot{
            .mode = "build",
            //.provider = "openai",
            //.model = "gpt-5.5",
            .session_id = "session_test",
            .input = "",
            .status = "ready",
            .transcript = {ava::tui::TranscriptItem{.label = "ava",
                                                    .text = "[https://example.test](https://example.test) [user@example.test](mailto:user@example.test) "
                                                            "[docs](https://example.test/docs)"}},
            .width = 120,
            .height = 12});
    auto link_fallback_plain = std::string{};
    for (auto const& line : link_fallback_transcript)
    {
      link_fallback_plain += strip_sgr(line);
      link_fallback_plain.push_back(' ');
    }
    expect(link_fallback_plain.find("https://example.test") != std::string::npos &&
               link_fallback_plain.find("https://example.test (https://example.test)") == std::string::npos &&
               link_fallback_plain.find("mailto:user@example.test") == std::string::npos &&
               link_fallback_plain.find("user@example.test") != std::string::npos &&
               link_fallback_plain.find("docs (https://example.test/docs)") != std::string::npos,
           "tui assistant renderer avoids duplicate URL/mailto fallback when Markdown link labels already equal targets");
  }

  {
    ScopedEnvVar no_color_hyperlink_guard("NO_COLOR", "");
    ScopedTerminalCapabilityProfile hyperlink_terminal("vscode");
    auto const osc8_link_transcript = ava::tui::render_composer(
        frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                        //.provider = "openai",
                                                        //.model = "gpt-5.5",
                                                        .session_id = "session_test",
                                                        .input = "",
                                                        .status = "ready",
                                                        .transcript = {ava::tui::TranscriptItem{
                                                            .label = "ava", .text = "[docs](https://example.test/docs) and https://example.test/bare"}},
                                                        .width = 120,
                                                        .height = 12});
    auto osc8_joined = std::string{};
    for (auto const& line : osc8_link_transcript)
      osc8_joined += line;
    expect(osc8_joined.find("\x1b]8;;https://example.test/docs\x1b\\") != std::string::npos &&
               osc8_joined.find("\x1b]8;;https://example.test/bare\x1b\\") != std::string::npos && osc8_joined.find("\x1b]8;;\x1b\\") != std::string::npos &&
               osc8_joined.find("(https://example.test/docs)") == std::string::npos,
           "tui assistant renderer emits Pi-style OSC 8 hyperlinks when terminal capabilities advertise them");
  }

  {
    ScopedEnvVar no_color_hyperlink_guard("NO_COLOR", "1");
    ScopedTerminalCapabilityProfile hyperlink_terminal("vscode");
    auto const plain_link_transcript = ava::tui::render_composer(
        frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                        //.provider = "openai",
                                                        //.model = "gpt-5.5",
                                                        .session_id = "session_test",
                                                        .input = "",
                                                        .status = "ready",
                                                        .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "[docs](https://example.test/docs)"}},
                                                        .width = 120,
                                                        .height = 12});
    auto plain_link_joined = std::string{};
    for (auto const& line : plain_link_transcript)
      plain_link_joined += line;
    expect(plain_link_joined.find("\x1b]8;;") == std::string::npos && plain_link_joined.find("docs (https://example.test/docs)") != std::string::npos,
           "plain TUI output suppresses OSC 8 hyperlinks and keeps visible link fallback text");
  }

  {
    ScopedTerminalCapabilityProfile no_hyperlink_terminal("");
    auto const bare_url_transcript = ava::tui::render_composer(
        frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                        //.provider = "openai",
                                                        //.model = "gpt-5.5",
                                                        .session_id = "session_test",
                                                        .input = "",
                                                        .status = "ready",
                                                        .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "Visit https://example.test/docs."}},
                                                        .width = 72,
                                                        .height = 12});
    auto const bare_url_line = std::ranges::find_if(
        bare_url_transcript, [](std::string const& line) { return strip_sgr(line).find("Visit https://example.test/docs.") != std::string::npos; });
    expect(bare_url_line != bare_url_transcript.end() && has_active_sgr_at_text(*bare_url_line, "https://example.test/docs", kUnderlineSgr) &&
               bare_url_line->find(std::string("https://example.test/docs") + std::string(kResetSgr) + ".") != std::string::npos &&
               strip_sgr(*bare_url_line).find("(https://example.test/docs)") == std::string::npos,
           "tui assistant renderer styles bare web URLs once and keeps trailing punctuation outside the link span");

    auto const bare_email_transcript = ava::tui::render_composer(
        frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                        //.provider = "openai",
                                                        //.model = "gpt-5.5",
                                                        .session_id = "session_test",
                                                        .input = "",
                                                        .status = "ready",
                                                        .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "Contact user@example.test."}},
                                                        .width = 72,
                                                        .height = 12});
    auto const bare_email_line = std::ranges::find_if(
        bare_email_transcript, [](std::string const& line) { return strip_sgr(line).find("Contact user@example.test.") != std::string::npos; });
    expect(bare_email_line != bare_email_transcript.end() && has_active_sgr_at_text(*bare_email_line, "user@example.test", kUnderlineSgr) &&
               bare_email_line->find(std::string("user@example.test") + std::string(kResetSgr) + ".") != std::string::npos &&
               strip_sgr(*bare_email_line).find("mailto:") == std::string::npos && strip_sgr(*bare_email_line).find("(user@example.test)") == std::string::npos,
           "tui assistant renderer styles bare email autolinks once without exposing a mailto fallback");
  }

  auto const wrapped_markdown_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.label = "ava",
                                                  .text = "- alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu\n> quoted alpha beta gamma "
                                                          "delta epsilon zeta eta theta iota kappa"}},
          .width = 48,
          .height = 16});
  auto const bullet_continuation =
      std::ranges::find_if(wrapped_markdown_transcript, [](std::string const& line) { return strip_sgr(line).find("  theta iota") != std::string::npos; });
  auto const quote_continuation =
      std::ranges::find_if(wrapped_markdown_transcript, [](std::string const& line) { return strip_sgr(line).find("│ eta theta") != std::string::npos; });
  auto const bullet_continuation_is_plain =
      bullet_continuation != wrapped_markdown_transcript.end() && !has_active_sgr_at_text(*bullet_continuation, "theta iota", kMutedSgr);
  auto const quote_continuation_is_plain =
      quote_continuation != wrapped_markdown_transcript.end() && !has_active_sgr_at_text(*quote_continuation, "eta theta", kMutedSgr);
  expect(bullet_continuation_is_plain && quote_continuation_is_plain, "tui assistant renderer keeps wrapped list and quote continuations out of code styling");

  auto const list_quote_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "- > alpha beta gamma delta epsilon zeta eta theta iota kappa\n>no-space quote"}},
          .width = 42,
          .height = 16});
  auto const list_quote_first =
      std::ranges::find_if(list_quote_transcript, [](std::string const& line) { return strip_sgr(line).find("- │ alpha beta") != std::string::npos; });
  auto const list_quote_wrap =
      std::ranges::find_if(list_quote_transcript, [](std::string const& line) { return strip_sgr(line).find("  │ zeta eta") != std::string::npos; });
  auto const no_space_quote =
      std::ranges::find_if(list_quote_transcript, [](std::string const& line) { return strip_sgr(line).find("│ no-space quote") != std::string::npos; });
  expect(list_quote_first != list_quote_transcript.end() && list_quote_wrap != list_quote_transcript.end() && no_space_quote != list_quote_transcript.end() &&
             std::ranges::all_of(list_quote_transcript, [](std::string const& line) { return visible_columns(line) <= 42; }),
         "tui assistant renderer uses Pi-style quote borders for plain and list-contained blockquotes");

  auto const lazy_blockquote_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = ">Foo\nbar with **bold** and `code`\n\nagain"}},
                                 .width = 58,
                                 .height = 14});
  auto const lazy_quote_foo =
      std::ranges::find_if(lazy_blockquote_transcript, [](std::string const& line) { return strip_sgr(line).find("│ Foo") != std::string::npos; });
  auto const lazy_quote_bar = std::ranges::find_if(
      lazy_blockquote_transcript, [](std::string const& line) { return strip_sgr(line).find("│ bar with bold and code") != std::string::npos; });
  auto const lazy_quote_after_blank = std::ranges::find_if(lazy_blockquote_transcript, [](std::string const& line) {
    auto const visible = strip_sgr(line);
    return visible.find("again") != std::string::npos && visible.find("│ again") == std::string::npos;
  });
  expect(lazy_quote_foo != lazy_blockquote_transcript.end() && lazy_quote_bar != lazy_blockquote_transcript.end() &&
             lazy_quote_after_blank != lazy_blockquote_transcript.end() && has_active_sgr_at_text(*lazy_quote_bar, "bold", kBoldSgr) &&
             has_active_sgr_at_text(*lazy_quote_bar, "code", kWarningSgr) &&
             std::ranges::all_of(lazy_blockquote_transcript, [](std::string const& line) { return visible_columns(line) <= 58; }),
         "tui assistant renderer applies Pi-style lazy blockquote continuation until a blank line");

  auto const divider_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "Before\n---\nMiddle\n* * *\nAfter\n___\nnot --- a rule"}},
                                 .width = 44,
                                 .height = 18});
  auto const divider_lines = std::ranges::count_if(divider_transcript, [](std::string const& line) {
    auto const visible = strip_sgr(line);
    return visible.find("─") != std::string::npos;
  });
  expect(divider_lines == 3 &&
             std::ranges::any_of(divider_transcript, [](std::string const& line) { return strip_sgr(line).find("not --- a rule") != std::string::npos; }) &&
             std::ranges::all_of(divider_transcript, [](std::string const& line) { return visible_columns(line) <= 44; }),
         "tui assistant renderer renders Markdown horizontal rules without treating ordinary dashed text as a divider");

  auto const task_list_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{
                                     .label = "ava", .text = "- [ ] alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu\n- [x] finished item"}},
                                 .width = 48,
                                 .height = 16});
  auto const task_unchecked =
      std::ranges::find_if(task_list_transcript, [](std::string const& line) { return strip_sgr(line).find("- [ ] alpha beta") != std::string::npos; });
  auto const task_checked =
      std::ranges::find_if(task_list_transcript, [](std::string const& line) { return strip_sgr(line).find("- [x] finished item") != std::string::npos; });
  auto const task_continuation =
      std::ranges::find_if(task_list_transcript, [](std::string const& line) { return strip_sgr(line).find("      eta theta") != std::string::npos; });
  expect(task_unchecked != task_list_transcript.end() && task_checked != task_list_transcript.end() && task_continuation != task_list_transcript.end() &&
             !has_active_sgr_at_text(*task_continuation, "eta theta", kMutedSgr) &&
             std::ranges::all_of(task_list_transcript, [](std::string const& line) { return visible_columns(line) <= 48; }),
         "tui assistant renderer preserves task-list markers and aligns wrapped task continuations");

  auto const ordered_list_transcript = ava::tui::render_composer(
      frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                      //.provider = "openai",
                                                      //.model = "gpt-5.5",
                                                      .session_id = "session_test",
                                                      .input = "",
                                                      .status = "ready",
                                                      .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "1. alpha\n1. beta\n1. gamma"}},
                                                      .width = 48,
                                                      .height = 14});
  auto const ordered_list_text = tui_test_support::join_visible_lines(ordered_list_transcript);
  expect(ordered_list_text.find("1. alpha") != std::string::npos && ordered_list_text.find("2. beta") != std::string::npos &&
             ordered_list_text.find("3. gamma") != std::string::npos && ordered_list_text.find("1. beta") == std::string::npos &&
             std::ranges::all_of(ordered_list_transcript, [](std::string const& line) { return visible_columns(line) <= 48; }),
         "tui assistant renderer normalizes repeated ordered-list markers Pi-style");

  auto const ordered_code_list_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{
                                     .label = "ava",
                                     .text = "1. First item\n\n```typescript\n// code block\n```\n\n1. Second item\n\n```typescript\n// another code "
                                             "block\n```\n\n1. Third item"}},
                                 .width = 72,
                                 .height = 24});
  auto const ordered_code_list_text = tui_test_support::join_visible_lines(ordered_code_list_transcript);
  expect(ordered_code_list_text.find("1. First item") != std::string::npos && ordered_code_list_text.find("2. Second item") != std::string::npos &&
             ordered_code_list_text.find("3. Third item") != std::string::npos && ordered_code_list_text.find("1. Second item") == std::string::npos &&
             ordered_code_list_text.find("``` typescript") != std::string::npos && ordered_code_list_text.find("// another code block") != std::string::npos &&
             std::ranges::all_of(ordered_code_list_transcript, [](std::string const& line) { return visible_columns(line) <= 72; }),
         "tui assistant renderer keeps ordered-list numbering across unindented fenced code blocks");

  auto assert_code_block_spacing = [&](std::string const& markdown) {
    auto const transcript =
        ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                  //.provider = "openai",
                                                                                  //.model = "gpt-5.5",
                                                                                  .session_id = "session_test",
                                                                                  .input = "",
                                                                                  .status = "ready",
                                                                                  .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = markdown}},
                                                                                  .width = 72,
                                                                                  .height = 18});
    std::vector<std::string> visible;
    visible.reserve(transcript.size());
    for (auto const& line : transcript)
    {
      auto stripped = strip_sgr(line);
      while (!stripped.empty() && stripped.back() == ' ')
        stripped.pop_back();
      visible.push_back(std::move(stripped));
    }
    auto const paragraph = std::ranges::find(visible, "  hello this is text");
    auto const index = paragraph == visible.end() ? visible.size() : static_cast<std::size_t>(paragraph - visible.begin());
    expect(index + 6 < visible.size() && visible[index + 1].empty() && visible[index + 2] == "  ```" && visible[index + 3] == "    code block" &&
               visible[index + 4] == "  ```" && visible[index + 5].empty() && visible[index + 6] == "  more text",
           "tui assistant renderer normalizes paragraph/fenced-code spacing to one blank line");
  };
  assert_code_block_spacing("hello this is text\n```\ncode block\n```\nmore text");
  assert_code_block_spacing("hello this is text\n\n```\ncode block\n```\n\nmore text");

  auto const heading_spacing_transcript = ava::tui::render_composer(
      frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                      //.provider = "openai",
                                                      //.model = "gpt-5.5",
                                                      .session_id = "session_test",
                                                      .input = "",
                                                      .status = "ready",
                                                      .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "# Hello\nThis is a paragraph"}},
                                                      .width = 72,
                                                      .height = 14});
  std::vector<std::string> heading_visible;
  heading_visible.reserve(heading_spacing_transcript.size());
  for (auto const& line : heading_spacing_transcript)
  {
    auto visible = strip_sgr(line);
    while (!visible.empty() && visible.back() == ' ')
      visible.pop_back();
    heading_visible.push_back(std::move(visible));
  }
  auto const heading_line = std::ranges::find(heading_visible, "  Hello");
  auto const heading_index = heading_line == heading_visible.end() ? heading_visible.size() : static_cast<std::size_t>(heading_line - heading_visible.begin());
  expect(
      heading_index + 2 < heading_visible.size() && heading_visible[heading_index + 1].empty() && heading_visible[heading_index + 2] == "  This is a paragraph",
      "tui assistant renderer normalizes heading/paragraph spacing to one blank line");

  auto const divider_spacing_transcript = ava::tui::render_composer(
      frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                      //.provider = "openai",
                                                      //.model = "gpt-5.5",
                                                      .session_id = "session_test",
                                                      .input = "",
                                                      .status = "ready",
                                                      .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "hello world\n---\nagain, hello world"}},
                                                      .width = 72,
                                                      .height = 14});
  std::vector<std::string> divider_visible;
  divider_visible.reserve(divider_spacing_transcript.size());
  for (auto const& line : divider_spacing_transcript)
  {
    auto visible = strip_sgr(line);
    while (!visible.empty() && visible.back() == ' ')
      visible.pop_back();
    divider_visible.push_back(std::move(visible));
  }
  auto const first_divider_line = std::ranges::find(divider_visible, "  hello world");
  auto const divider_index =
      first_divider_line == divider_visible.end() ? divider_visible.size() : static_cast<std::size_t>(first_divider_line - divider_visible.begin());
  expect(divider_index + 3 < divider_visible.size() && divider_visible[divider_index + 1].find("─") != std::string::npos &&
             divider_visible[divider_index + 2].empty() && divider_visible[divider_index + 3] == "  again, hello world",
         "tui assistant renderer normalizes divider/paragraph spacing to one blank line");

  auto const html_literal_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.label = "ava",
                                                  .text = "This is text with <thinking>hidden content</thinking> that should stay visible.\n```html\n<div>Some "
                                                          "HTML</div>\n```"}},
          .width = 88,
          .height = 18});
  auto const html_literal_text = tui_test_support::join_visible_lines(html_literal_transcript);
  expect(
      html_literal_text.find("<thinking>hidden content</thinking>") != std::string::npos && html_literal_text.find("<div>Some HTML</div>") != std::string::npos,
      "tui assistant renderer keeps HTML-like tags visible in prose and fenced code");

  auto const loose_list_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.label = "ava",
                                                  .text = "1. Lorem ipsum dolor sit amet.\n\n   Ut enim ad minim veniam.\n\n1. Duis aute irure dolor.\n\n   "
                                                          "Excepteur sint occaecat cupidatat.\n\n1. Beep boop"}},
          .width = 88,
          .height = 24});
  std::vector<std::string> loose_list_visible;
  loose_list_visible.reserve(loose_list_transcript.size());
  for (auto const& line : loose_list_transcript)
  {
    auto visible = strip_sgr(line);
    while (!visible.empty() && visible.back() == ' ')
      visible.pop_back();
    loose_list_visible.push_back(std::move(visible));
  }
  auto const first_loose_item = std::ranges::find(loose_list_visible, "  1. Lorem ipsum dolor sit amet.");
  auto const loose_index =
      first_loose_item == loose_list_visible.end() ? loose_list_visible.size() : static_cast<std::size_t>(first_loose_item - loose_list_visible.begin());
  expect(loose_index + 8 < loose_list_visible.size() && loose_list_visible[loose_index + 1].empty() &&
             loose_list_visible[loose_index + 2] == "     Ut enim ad minim veniam." && loose_list_visible[loose_index + 3].empty() &&
             loose_list_visible[loose_index + 4] == "  2. Duis aute irure dolor." && loose_list_visible[loose_index + 5].empty() &&
             loose_list_visible[loose_index + 6] == "     Excepteur sint occaecat cupidatat." && loose_list_visible[loose_index + 7].empty() &&
             loose_list_visible[loose_index + 8] == "  3. Beep boop",
         "tui assistant renderer preserves Pi-style loose ordered-list continuations and numbering");

  auto const nested_list_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.label = "ava",
                                                  .text = "- parent\n  - alpha beta gamma delta epsilon zeta eta theta iota\n    + grand child\n1. ordered\n   "
                                                          "1) nested ordered"}},
          .width = 48,
          .height = 18});
  auto const nested_unordered =
      std::ranges::find_if(nested_list_transcript, [](std::string const& line) { return strip_sgr(line).find("    - alpha beta") != std::string::npos; });
  auto const nested_continuation =
      std::ranges::find_if(nested_list_transcript, [](std::string const& line) { return strip_sgr(line).find("      eta theta") != std::string::npos; });
  auto const nested_plus =
      std::ranges::find_if(nested_list_transcript, [](std::string const& line) { return strip_sgr(line).find("        + grand child") != std::string::npos; });
  auto const nested_ordered =
      std::ranges::find_if(nested_list_transcript, [](std::string const& line) { return strip_sgr(line).find("    1) nested ordered") != std::string::npos; });
  expect(nested_unordered != nested_list_transcript.end() && nested_continuation != nested_list_transcript.end() &&
             nested_plus != nested_list_transcript.end() && nested_ordered != nested_list_transcript.end() &&
             !has_active_sgr_at_text(*nested_continuation, "eta theta", kMutedSgr) &&
             std::ranges::all_of(nested_list_transcript, [](std::string const& line) { return visible_columns(line) <= 48; }),
         "tui assistant renderer preserves Pi-style nested list indentation and wrapped continuations");

  auto const table_transcript = ava::tui::render_composer(
      frontend.frontend(), ava::tui::ComposerSnapshot{
                               .mode = "build",
                               //.provider = "openai",
                               //.model = "gpt-5.5",
                               .session_id = "session_test",
                               .input = "",
                               .status = "ready",
                               .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "| Name | Age |\n| --- | ---: |\n| Alice | 30 |\n| Bob | 25 |"}},
                               .width = 64,
                               .height = 16});
  auto const table_header = std::ranges::find_if(table_transcript, [](std::string const& line) {
    auto const visible = strip_sgr(line);
    return visible.find("│ Name") != std::string::npos && visible.find("Age │") != std::string::npos;
  });
  auto const table_alice = std::ranges::find_if(table_transcript, [](std::string const& line) {
    auto const visible = strip_sgr(line);
    return visible.find("│ Alice") != std::string::npos && visible.find(" 30 │") != std::string::npos;
  });
  auto const table_top_border = std::ranges::find_if(table_transcript, [](std::string const& line) {
    auto const visible = strip_sgr(line);
    return visible.find("┌") != std::string::npos && visible.find("┬") != std::string::npos && visible.find("┐") != std::string::npos;
  });
  auto const table_bottom_border = std::ranges::find_if(table_transcript, [](std::string const& line) {
    auto const visible = strip_sgr(line);
    return visible.find("└") != std::string::npos && visible.find("┴") != std::string::npos && visible.find("┘") != std::string::npos;
  });
  auto const table_dividers = std::ranges::count_if(table_transcript, [](std::string const& line) { return strip_sgr(line).find("┼") != std::string::npos; });
  expect(table_header != table_transcript.end() && table_alice != table_transcript.end() && table_top_border != table_transcript.end() &&
             table_bottom_border != table_transcript.end() && table_dividers == 2 &&
             std::ranges::any_of(table_transcript, [](std::string const& line) { return strip_sgr(line).find("─") != std::string::npos; }) &&
             std::ranges::all_of(table_transcript, [](std::string const& line) { return visible_columns(line) <= 64; }),
         "tui assistant renderer renders simple Markdown tables with Pi-style outer borders, dividers, and right alignment");

  auto const too_narrow_table_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "| A | B | C | D | E |\n| --- | --- | --- | --- | --- |\n| 1 | 2 | 3 | 4 | 5 |"}},
          .width = 20,
          .height = 17});
  auto const too_narrow_text =
      std::accumulate(too_narrow_table_transcript.begin(), too_narrow_table_transcript.end(), std::string{}, [](std::string acc, std::string const& line) {
        acc += " ";
        acc += strip_sgr(line);
        return acc;
      });
  expect(too_narrow_text.find("| A | B | C") != std::string::npos && too_narrow_text.find("| 1 | 2 | 3") != std::string::npos &&
             std::ranges::none_of(too_narrow_table_transcript,
                                  [](std::string const& line) {
                                    auto const visible = strip_sgr(line);
                                    auto const quiet_composer_row = visible.starts_with("│  Type a message...") || visible.starts_with("│  GPT-") ||
                                                                    (visible.starts_with("│") && visible.find_first_not_of("│ ") == std::string::npos);
                                    return !quiet_composer_row && (visible.find("┌") != std::string::npos || visible.find("│") != std::string::npos ||
                                                                   visible.find("└") != std::string::npos);
                                  }) &&
             std::ranges::all_of(too_narrow_table_transcript, [](std::string const& line) { return visible_columns(line) <= 20; }),
         "tui assistant renderer falls back to wrapped raw Markdown when a table is too narrow for stable borders");

  auto const narrow_table_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{
              .label = "ava", .text = "| Command | Description |\n| --- | --- |\n| npm install | Install all dependencies for the workspace |"}},
          .width = 40,
          .height = 18});
  auto const narrow_table_text =
      std::accumulate(narrow_table_transcript.begin(), narrow_table_transcript.end(), std::string{}, [](std::string acc, std::string const& line) {
        acc += " ";
        acc += strip_sgr(line);
        return acc;
      });
  expect(narrow_table_text.find("Command") != std::string::npos && narrow_table_text.find("npm install") != std::string::npos &&
             narrow_table_text.find("dependencies") != std::string::npos && narrow_table_text.find("workspace") != std::string::npos &&
             std::ranges::any_of(narrow_table_transcript, [](std::string const& line) { return strip_sgr(line).find("│ Command") != std::string::npos; }) &&
             std::ranges::all_of(narrow_table_transcript, [](std::string const& line) { return visible_columns(line) <= 40; }),
         "tui assistant renderer wraps Markdown table cells while preserving borders and content");

  auto const one_column_table_transcript = ava::tui::render_composer(
      frontend.frontend(), ava::tui::ComposerSnapshot{
                               .mode = "build",
                               //.provider = "openai",
                               //.model = "gpt-5.5",
                               .session_id = "session_test",
                               .input = "",
                               .status = "ready",
                               .transcript = {ava::tui::TranscriptItem{.label = "ava",
                                                                       .text = "| Header |\n| --- |\n| This is a very long cell content that should wrap |\n| "
                                                                               "prefix https://example.com/this/is/a/very/long/url/that/should/wrap |"}},
                               .width = 36,
                               .height = 25});
  auto count_table_borders = [](std::string const& line) {
    auto const visible = strip_sgr(line);
    auto count = std::size_t{0};
    for (auto found = visible.find("│"); found != std::string::npos; found = visible.find("│", found + 1))
    {
      ++count;
    }
    return count;
  };
  auto one_column_table_text = std::string{};
  for (auto const& line : one_column_table_transcript)
  {
    auto const visible = strip_sgr(line);
    for (std::size_t index = 0; index < visible.size();)
    {
      if (visible.compare(index, std::string_view("│").size(), "│") == 0 || visible.compare(index, std::string_view("├").size(), "├") == 0 ||
          visible.compare(index, std::string_view("┤").size(), "┤") == 0 || visible.compare(index, std::string_view("┼").size(), "┼") == 0 ||
          visible.compare(index, std::string_view("─").size(), "─") == 0)
      {
        index += std::string_view("│").size();
        continue;
      }
      if (visible[index] != ' ')
        one_column_table_text.push_back(visible[index]);
      ++index;
    }
  }
  expect(std::ranges::any_of(one_column_table_transcript, [](std::string const& line) { return strip_sgr(line).find("│ Header") != std::string::npos; }) &&
             one_column_table_text.find("verylongcellcontent") != std::string::npos &&
             one_column_table_text.find("prefixhttps://example.com/this/is/a/very/long/url/that/should/wrap") != std::string::npos &&
             std::ranges::all_of(one_column_table_transcript, [](std::string const& line) { return visible_columns(line) <= 36; }) &&
             std::ranges::all_of(one_column_table_transcript,
                                 [&](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("│") == std::string::npos || visible.starts_with("│  Type a message...") ||
                                          visible.starts_with("│  GPT-") ||
                                          (visible.starts_with("│") && visible.find_first_not_of("│ ") == std::string::npos) || count_table_borders(line) == 2;
                                 }),
         "tui assistant renderer supports one-column tables and wraps long unbroken table tokens without losing borders");

  auto const styled_table_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "| Code |\n| --- |\n| `averyveryveryverylongidentifier` |"}},
                                 .width = 36,
                                 .height = 19});
  auto styled_table_text = std::string{};
  for (auto const& line : styled_table_transcript)
  {
    auto const visible = strip_sgr(line);
    for (std::size_t index = 0; index < visible.size();)
    {
      if (visible.compare(index, std::string_view("│").size(), "│") == 0 || visible.compare(index, std::string_view("├").size(), "├") == 0 ||
          visible.compare(index, std::string_view("┤").size(), "┤") == 0 || visible.compare(index, std::string_view("┼").size(), "┼") == 0 ||
          visible.compare(index, std::string_view("─").size(), "─") == 0)
      {
        index += std::string_view("│").size();
        continue;
      }
      if (visible[index] != ' ')
        styled_table_text.push_back(visible[index]);
      ++index;
    }
  }
  expect(std::ranges::any_of(styled_table_transcript, [&kWarningSgr](std::string const& line) { return has_active_sgr_at_text(line, "avery", kWarningSgr); }) &&
             styled_table_text.find("averyveryveryverylongidentifier") != std::string::npos && styled_table_text.find("`") == std::string::npos &&
             std::ranges::all_of(styled_table_transcript, [](std::string const& line) { return visible_columns(line) <= 36; }) &&
             std::ranges::all_of(styled_table_transcript,
                                 [&](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("│") == std::string::npos || visible.starts_with("│  Type a message...") ||
                                          visible.starts_with("│  GPT-") ||
                                          (visible.starts_with("│") && visible.find_first_not_of("│ ") == std::string::npos) || count_table_borders(line) == 2;
                                 }),
         "tui assistant renderer wraps styled inline code inside table cells without breaking borders");

  auto const narrow_three_column_table = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "| A | B | C |\n| --- | --- | --- |\n| 1 | 2 | 3 |"}},
                                 .width = 22,
                                 .height = 14});
  auto const narrow_three_column_text = tui_test_support::join_visible_lines(narrow_three_column_table);
  expect(!narrow_three_column_table.empty() && narrow_three_column_text.find("A") != std::string::npos &&
             narrow_three_column_text.find("B") != std::string::npos && narrow_three_column_text.find("C") != std::string::npos &&
             narrow_three_column_text.find("1") != std::string::npos && narrow_three_column_text.find("2") != std::string::npos &&
             narrow_three_column_text.find("3") != std::string::npos &&
             std::ranges::any_of(narrow_three_column_table, [](std::string const& line) { return strip_sgr(line).find("│ A") != std::string::npos; }) &&
             std::ranges::all_of(narrow_three_column_table, [](std::string const& line) { return visible_columns(line) <= 22; }),
         "tui assistant renderer keeps narrow multi-column Markdown tables bounded and readable");

  auto const highlighted_code_transcript = ava::tui::render_composer(
      frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                      //.provider = "openai",
                                                      //.model = "gpt-5.5",
                                                      .session_id = "session_test",
                                                      .input = "",
                                                      .status = "ready",
                                                      .transcript = {ava::tui::TranscriptItem{.label = "ava",
                                                                                              .text = "```cpp\n"
                                                                                                      "int main() {\n"
                                                                                                      "  std::string value = \"ok\";\n"
                                                                                                      "  return 42;\n"
                                                                                                      "}\n"
                                                                                                      "```\n"
                                                                                                      "```diff\n"
                                                                                                      "@@ -1 +1 @@\n"
                                                                                                      "-old\n"
                                                                                                      "+new\n"
                                                                                                      " context\n"
                                                                                                      "```\n"
                                                                                                      "```json\n"
                                                                                                      "{\"ok\": true, \"n\": 12}\n"
                                                                                                      "```\n"
                                                                                                      "```sh\n"
                                                                                                      "echo \"ok\" # comment\n"
                                                                                                      "```\n"
                                                                                                      "```typescript\n"
                                                                                                      "interface User { name: string; }\n"
                                                                                                      "const greet = (user: User): string => \"hi\";\n"
                                                                                                      "```\n"
                                                                                                      "```javascript\n"
                                                                                                      "async function load() {\n"
                                                                                                      "  return await fetch(\"/ok\");\n"
                                                                                                      "}\n"
                                                                                                      "```\n"
                                                                                                      "```html\n"
                                                                                                      "<div class=\"ok\">Hi</div>\n"
                                                                                                      "<!-- note -->\n"
                                                                                                      "```\n"
                                                                                                      "```css\n"
                                                                                                      ".card { color: #fff; margin: 12px; }\n"
                                                                                                      "```\n"
                                                                                                      "```yaml\n"
                                                                                                      "enabled: true\n"
                                                                                                      "name: \"ava\"\n"
                                                                                                      "count: 42\n"
                                                                                                      "# comment\n"
                                                                                                      "```\n"
                                                                                                      "```cmake\n"
                                                                                                      "add_executable(ava main.cpp)\n"
                                                                                                      "set(AVA_ENABLED ON)\n"
                                                                                                      "# build comment\n"
                                                                                                      "```\n"
                                                                                                      "```toml\n"
                                                                                                      "[tool.ava]\n"
                                                                                                      "name = \"ava\"\n"
                                                                                                      "enabled = true\n"
                                                                                                      "count = 42\n"
                                                                                                      "# note\n"
                                                                                                      "```\n"
                                                                                                      "```ini\n"
                                                                                                      "[server]\n"
                                                                                                      "port = 8080\n"
                                                                                                      "enabled = yes\n"
                                                                                                      "; note\n"
                                                                                                      "```\n"
                                                                                                      "```text\n"
                                                                                                      "return \"plain\";\n"
                                                                                                      "```"}},
                                                      .width = 72,
                                                      .height = 120});
  auto const cpp_signature =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("int main() {") != std::string::npos; });
  auto const cpp_string = std::ranges::find_if(
      highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("std::string value = \"ok\";") != std::string::npos; });
  auto const cpp_return =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("return 42;") != std::string::npos; });
  auto const diff_hunk =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("@@ -1 +1 @@") != std::string::npos; });
  auto const diff_removed =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("-old") != std::string::npos; });
  auto const diff_added =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("+new") != std::string::npos; });
  auto const json_code = std::ranges::find_if(highlighted_code_transcript,
                                              [](std::string const& line) { return strip_sgr(line).find("{\"ok\": true, \"n\": 12}") != std::string::npos; });
  auto const shell_code = std::ranges::find_if(highlighted_code_transcript,
                                               [](std::string const& line) { return strip_sgr(line).find("echo \"ok\" # comment") != std::string::npos; });
  auto const typescript_interface = std::ranges::find_if(
      highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("interface User { name: string; }") != std::string::npos; });
  auto const typescript_const = std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) {
    return strip_sgr(line).find("const greet = (user: User): string => \"hi\";") != std::string::npos;
  });
  auto const javascript_async = std::ranges::find_if(
      highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("async function load() {") != std::string::npos; });
  auto const javascript_return = std::ranges::find_if(
      highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("return await fetch(\"/ok\");") != std::string::npos; });
  auto const html_tag = std::ranges::find_if(highlighted_code_transcript,
                                             [](std::string const& line) { return strip_sgr(line).find("<div class=\"ok\">Hi</div>") != std::string::npos; });
  auto const html_comment =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("<!-- note -->") != std::string::npos; });
  auto const css_code = std::ranges::find_if(
      highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find(".card { color: #fff; margin: 12px; }") != std::string::npos; });
  auto const yaml_enabled =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("enabled: true") != std::string::npos; });
  auto const yaml_name =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("name: \"ava\"") != std::string::npos; });
  auto const yaml_count =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("count: 42") != std::string::npos; });
  auto const yaml_comment =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("# comment") != std::string::npos; });
  auto const cmake_command = std::ranges::find_if(
      highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("add_executable(ava main.cpp)") != std::string::npos; });
  auto const cmake_option = std::ranges::find_if(highlighted_code_transcript,
                                                 [](std::string const& line) { return strip_sgr(line).find("set(AVA_ENABLED ON)") != std::string::npos; });
  auto const cmake_comment =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("# build comment") != std::string::npos; });
  auto const toml_header =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("[tool.ava]") != std::string::npos; });
  auto const toml_name =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("name = \"ava\"") != std::string::npos; });
  auto const toml_enabled =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("enabled = true") != std::string::npos; });
  auto const toml_count =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("count = 42") != std::string::npos; });
  auto const toml_comment =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("# note") != std::string::npos; });
  auto const ini_header =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("[server]") != std::string::npos; });
  auto const ini_port =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("port = 8080") != std::string::npos; });
  auto const ini_enabled =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("enabled = yes") != std::string::npos; });
  auto const ini_comment =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("; note") != std::string::npos; });
  auto const fallback_code =
      std::ranges::find_if(highlighted_code_transcript, [](std::string const& line) { return strip_sgr(line).find("return \"plain\";") != std::string::npos; });
  auto const found_highlight_examples =
      cpp_signature != highlighted_code_transcript.end() && cpp_string != highlighted_code_transcript.end() &&
      cpp_return != highlighted_code_transcript.end() && diff_hunk != highlighted_code_transcript.end() && diff_removed != highlighted_code_transcript.end() &&
      diff_added != highlighted_code_transcript.end() && json_code != highlighted_code_transcript.end() && shell_code != highlighted_code_transcript.end() &&
      typescript_interface != highlighted_code_transcript.end() && typescript_const != highlighted_code_transcript.end() &&
      javascript_async != highlighted_code_transcript.end() && javascript_return != highlighted_code_transcript.end() &&
      html_tag != highlighted_code_transcript.end() && html_comment != highlighted_code_transcript.end() && css_code != highlighted_code_transcript.end() &&
      yaml_enabled != highlighted_code_transcript.end() && yaml_name != highlighted_code_transcript.end() && yaml_count != highlighted_code_transcript.end() &&
      yaml_comment != highlighted_code_transcript.end() && cmake_command != highlighted_code_transcript.end() &&
      cmake_option != highlighted_code_transcript.end() && cmake_comment != highlighted_code_transcript.end() &&
      toml_header != highlighted_code_transcript.end() && toml_name != highlighted_code_transcript.end() && toml_enabled != highlighted_code_transcript.end() &&
      toml_count != highlighted_code_transcript.end() && toml_comment != highlighted_code_transcript.end() && ini_header != highlighted_code_transcript.end() &&
      ini_port != highlighted_code_transcript.end() && ini_enabled != highlighted_code_transcript.end() && ini_comment != highlighted_code_transcript.end() &&
      fallback_code != highlighted_code_transcript.end();
  expect(found_highlight_examples, "tui assistant renderer keeps all fenced-code highlight examples visible");
  if (found_highlight_examples)
  {
    expect(has_active_sgr_at_text(*cpp_signature, "int", kWarningSgr) && has_active_sgr_at_text(*cpp_signature, "main", kAccentSgr) &&
               has_active_sgr_at_text(*cpp_string, "\"ok\"", kSuccessSgr) && has_active_sgr_at_text(*cpp_return, "return", kWarningSgr) &&
               has_active_sgr_at_text(*cpp_return, "42", kAccentSgr),
           "tui assistant renderer applies lightweight C++ syntax coloring");
    expect(has_active_sgr_at_text(*diff_hunk, "@@ -1 +1 @@", kAccentSgr) && has_active_sgr_at_text(*diff_removed, "-old", kErrorSgr) &&
               has_active_sgr_at_text(*diff_added, "+new", kSuccessSgr),
           "tui assistant renderer applies diff syntax coloring");
    expect(has_active_sgr_at_text(*json_code, "\"ok\"", kSuccessSgr) && has_active_sgr_at_text(*json_code, "true", kWarningSgr) &&
               has_active_sgr_at_text(*json_code, "12", kAccentSgr),
           "tui assistant renderer applies JSON syntax coloring");
    expect(has_active_sgr_at_text(*shell_code, "\"ok\"", kSuccessSgr) && has_active_sgr_at_text(*shell_code, "# comment", kMutedSgr),
           "tui assistant renderer applies shell syntax coloring");
    expect(has_active_sgr_at_text(*typescript_interface, "interface", kWarningSgr) && has_active_sgr_at_text(*typescript_interface, "string", kWarningSgr) &&
               has_active_sgr_at_text(*typescript_const, "const", kWarningSgr) && has_active_sgr_at_text(*typescript_const, "\"hi\"", kSuccessSgr),
           "tui assistant renderer applies TypeScript syntax coloring for Pi-style language names");
    expect(has_active_sgr_at_text(*javascript_async, "async", kWarningSgr) && has_active_sgr_at_text(*javascript_async, "function", kWarningSgr) &&
               has_active_sgr_at_text(*javascript_async, "load", kAccentSgr) && has_active_sgr_at_text(*javascript_return, "return", kWarningSgr) &&
               has_active_sgr_at_text(*javascript_return, "await", kWarningSgr) && has_active_sgr_at_text(*javascript_return, "fetch", kAccentSgr) &&
               has_active_sgr_at_text(*javascript_return, "\"/ok\"", kSuccessSgr),
           "tui assistant renderer applies JavaScript syntax coloring for Pi-style language names");
    expect(has_active_sgr_at_text(*html_tag, "div", kAccentSgr) && has_active_sgr_at_text(*html_tag, "class", kWarningSgr) &&
               has_active_sgr_at_text(*html_tag, "\"ok\"", kSuccessSgr) && has_active_sgr_at_text(*html_comment, "<!-- note -->", kMutedSgr),
           "tui assistant renderer applies HTML syntax coloring");
    expect(has_active_sgr_at_text(*css_code, "color", kWarningSgr) && has_active_sgr_at_text(*css_code, "#fff", kAccentSgr) &&
               has_active_sgr_at_text(*css_code, "margin", kWarningSgr) && has_active_sgr_at_text(*css_code, "12px", kAccentSgr),
           "tui assistant renderer applies CSS syntax coloring");
    expect(has_active_sgr_at_text(*yaml_enabled, "enabled", kWarningSgr), "tui assistant renderer highlights YAML keys");
    expect(has_active_sgr_at_text(*yaml_enabled, "true", kWarningSgr), "tui assistant renderer highlights YAML booleans");
    expect(has_active_sgr_at_text(*yaml_name, "name", kWarningSgr) && has_active_sgr_at_text(*yaml_name, "\"ava\"", kSuccessSgr),
           "tui assistant renderer highlights YAML string values");
    expect(has_active_sgr_at_text(*yaml_count, "count", kWarningSgr), "tui assistant renderer highlights YAML numeric keys");
    expect(has_active_sgr_at_text(*yaml_count, "42", kAccentSgr), "tui assistant renderer highlights YAML numeric scalar values");
    expect(has_active_sgr_at_text(*yaml_comment, "# comment", kMutedSgr), "tui assistant renderer highlights YAML comments");
    expect(has_active_sgr_at_text(*cmake_command, "add_executable", kAccentSgr) && has_active_sgr_at_text(*cmake_option, "set", kAccentSgr) &&
               has_active_sgr_at_text(*cmake_option, "ON", kWarningSgr) && has_active_sgr_at_text(*cmake_comment, "# build comment", kMutedSgr),
           "tui assistant renderer highlights CMake commands, scalars, and comments");
    expect(has_active_sgr_at_text(*toml_header, "tool.ava", kAccentSgr) && has_active_sgr_at_text(*toml_name, "name", kWarningSgr) &&
               has_active_sgr_at_text(*toml_name, "\"ava\"", kSuccessSgr) && has_active_sgr_at_text(*toml_enabled, "enabled", kWarningSgr) &&
               has_active_sgr_at_text(*toml_enabled, "true", kWarningSgr) && has_active_sgr_at_text(*toml_count, "42", kAccentSgr) &&
               has_active_sgr_at_text(*toml_comment, "# note", kMutedSgr),
           "tui assistant renderer highlights TOML table headers, keys, scalars, and comments");
    expect(has_active_sgr_at_text(*ini_header, "server", kAccentSgr) && has_active_sgr_at_text(*ini_port, "port", kWarningSgr) &&
               has_active_sgr_at_text(*ini_port, "8080", kAccentSgr) && has_active_sgr_at_text(*ini_enabled, "enabled", kWarningSgr) &&
               has_active_sgr_at_text(*ini_enabled, "yes", kWarningSgr) && has_active_sgr_at_text(*ini_comment, "; note", kMutedSgr),
           "tui assistant renderer highlights INI sections, keys, scalars, and comments");
    expect(has_active_sgr_at_text(*fallback_code, "return", kMutedSgr) && !has_active_sgr_at_text(*fallback_code, "return", kWarningSgr),
           "tui assistant renderer preserves dim plain fallback for unsupported code fences");
  }
  expect(std::ranges::all_of(highlighted_code_transcript, [](std::string const& line) { return visible_columns(line) <= 72; }),
         "tui assistant renderer keeps highlighted code lines width-safe");

  auto const wrapped_code_fence_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{
                                     .label = "ava", .text = std::string("```text\n") + std::string(42, 'a') + "```literal\nomega\n```\nAfter **bold**"}},
                                 .width = 48,
                                 .height = 16});
  auto const code_after_wrapped_ticks =
      std::ranges::find_if(wrapped_code_fence_transcript, [](std::string const& line) { return strip_sgr(line).find("  omega") != std::string::npos; });
  auto const text_after_code =
      std::ranges::find_if(wrapped_code_fence_transcript, [](std::string const& line) { return strip_sgr(line).find("After bold") != std::string::npos; });
  expect(code_after_wrapped_ticks != wrapped_code_fence_transcript.end() && has_active_sgr_at_text(*code_after_wrapped_ticks, "omega", kMutedSgr) &&
             text_after_code != wrapped_code_fence_transcript.end() && !has_active_sgr_at_text(*text_after_code, "After", kMutedSgr) &&
             has_active_sgr_at_text(*text_after_code, "bold", kBoldSgr),
         "tui assistant renderer keeps wrapped code content beginning with backticks inside the code block");

  auto const indented_fence_content_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "```text\n  ```literal\nomega\n```\nAfter **bold**"}},
                                 .width = 56,
                                 .height = 16});
  auto const indented_ticks =
      std::ranges::find_if(indented_fence_content_transcript, [](std::string const& line) { return strip_sgr(line).find("```literal") != std::string::npos; });
  auto const code_after_indented_ticks =
      std::ranges::find_if(indented_fence_content_transcript, [](std::string const& line) { return strip_sgr(line).find("  omega") != std::string::npos; });
  auto const text_after_indented_code =
      std::ranges::find_if(indented_fence_content_transcript, [](std::string const& line) { return strip_sgr(line).find("After bold") != std::string::npos; });
  expect(indented_ticks != indented_fence_content_transcript.end() && strip_sgr(*indented_ticks).find("``` literal") == std::string::npos &&
             code_after_indented_ticks != indented_fence_content_transcript.end() && has_active_sgr_at_text(*code_after_indented_ticks, "omega", kMutedSgr) &&
             text_after_indented_code != indented_fence_content_transcript.end() && !has_active_sgr_at_text(*text_after_indented_code, "After", kMutedSgr) &&
             has_active_sgr_at_text(*text_after_indented_code, "bold", kBoldSgr),
         "tui assistant renderer keeps indented backtick content inside fenced code blocks");

  auto const narrow_code_transcript = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "Intro `ok` and **bold**.\n```text\nvalue `x` and **y**\n```\nDone **bold**."}},
          .width = 30,
          .height = 18});
  auto const narrow_inline =
      std::ranges::find_if(narrow_code_transcript, [](std::string const& line) { return strip_sgr(line).find("Intro ok and bold.") != std::string::npos; });
  auto const narrow_code =
      std::ranges::find_if(narrow_code_transcript, [](std::string const& line) { return strip_sgr(line).find("value `x` and **y**") != std::string::npos; });
  auto const narrow_after_code =
      std::ranges::find_if(narrow_code_transcript, [](std::string const& line) { return strip_sgr(line).find("Done bold.") != std::string::npos; });
  expect(narrow_inline != narrow_code_transcript.end() && has_active_sgr_at_text(*narrow_inline, "ok", kWarningSgr) &&
             has_active_sgr_at_text(*narrow_inline, "bold", kBoldSgr) && narrow_code != narrow_code_transcript.end() &&
             !has_active_sgr_at_text(*narrow_code, "x", kWarningSgr) && !has_active_sgr_at_text(*narrow_code, "y", kBoldSgr) &&
             narrow_after_code != narrow_code_transcript.end() && has_active_sgr_at_text(*narrow_after_code, "bold", kBoldSgr),
         "tui narrow assistant renderer keeps code literal while formatting inline markdown outside code");

  auto const narrow_transcript = ava::tui::render_composer(
      frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                      //.provider = "openai",
                                                      //.model = "gpt-5.5",
                                                      .session_id = "session_test",
                                                      .input = "",
                                                      .status = "ready",
                                                      .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = std::string(64, 'x') + " done"}},
                                                      .width = 20,
                                                      .height = 14});
  expect(std::ranges::any_of(narrow_transcript, [](std::string const& line) { return strip_sgr(line).find("xxx") != std::string::npos; }) &&
             std::ranges::all_of(narrow_transcript, [](std::string const& line) { return visible_columns(line) <= 20; }),
         "tui assistant renderer keeps long words readable at narrow widths");
}
namespace {
void test_tui_text_model_conversions()
{
  tui_test_support::TuiFrontEndFixture frontend;
  auto const plain = ava::tui::text_from_plain("first\r\nsecond\nthird\rfour");
  expect(ava::tui::to_plain_text(plain) == "first\nsecond\nthird\nfour" && ava::tui::validate_text(plain).has_value(),
         "tui Text plain conversion normalizes CR/LF boundaries into explicit newline runs");

  auto appended = ava::tui::Text{};
  auto ok_string = ava::tui::append_string(appended, "plain");
  ava::tui::append_newline(appended);
  auto ok_span = ava::tui::append_span(appended, "code", ava::tui::Rendition{.code = true});
  auto bad_string = ava::tui::append_string(appended, "bad\nrun");
  auto bad_span = ava::tui::append_span(appended, "bad\rrun", ava::tui::Rendition{.bold = true});
  expect(ok_string.has_value() && ok_span.has_value() && !bad_string.has_value() && !bad_span.has_value() && ava::tui::to_plain_text(appended) == "plain\ncode",
         "tui Text builders reject embedded newlines inside string and span runs");

  auto invalid = ava::tui::Text{};
  invalid.runs.push_back(ava::tui::String{.text = "broken\nrun"});
  expect(!ava::tui::validate_text(invalid).has_value(), "tui Text validation catches invalid hand-built string runs");

  auto const markdown = ava::tui::text_from_markdown(
      "# Title\nSee [docs](https://example.test) and *note*.\nUse `ava` and **bold**.\n```cpp\nint main() "
      "{}\n```\nDrop ~~old~~ but keep ~literal~.\n**open");
  bool saw_code = false;
  bool saw_bold = false;
  bool saw_italic = false;
  bool saw_link = false;
  bool saw_strikethrough = false;
  for (auto const& run : markdown.runs)
  {
    if (auto const* span = std::get_if<ava::tui::TextSpan>(&run))
    {
      saw_code = saw_code || span->rendition.code;
      saw_bold = saw_bold || span->rendition.bold;
      saw_italic = saw_italic || span->rendition.italic;
      saw_link = saw_link || (span->rendition.underline && span->rendition.color == ava::tui::TextColorRole::Accent);
      saw_strikethrough = saw_strikethrough || span->rendition.strikethrough;
    }
  }
  auto const markdown_plain = ava::tui::to_plain_text(markdown);
  expect(ava::tui::validate_text(markdown).has_value() && saw_code && saw_bold && saw_italic && saw_link && saw_strikethrough &&
             markdown_plain.find("Title") != std::string::npos && markdown_plain.find("docs (https://example.test)") != std::string::npos &&
             markdown_plain.find("Use ava and bold.") != std::string::npos && markdown_plain.find("Drop old but keep ~literal~.") != std::string::npos &&
             markdown_plain.find("~~old~~") == std::string::npos && markdown_plain.find("**open") != std::string::npos,
         "tui Markdown conversion supports basic heading/link/emphasis/code/fences and leaves unsupported Markdown readable");

  auto const rendered_from_model = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_text_model",
          .input = "",
          .status = "ready",
          .transcript =
              {ava::tui::TranscriptItem{.label = "you", .text = "raw-user-hidden", .text_model = ava::tui::text_from_plain("model user visible")},
               ava::tui::TranscriptItem{.label = "ava", .text = "", .text_model = ava::tui::text_from_plain("model assistant visible")},
               ava::tui::TranscriptItem{
                   .label = "ava", .text = "answer", .thinking = "raw-thinking-hidden", .thinking_model = ava::tui::text_from_plain("model thinking visible")},
               ava::tui::TranscriptItem{.label = "error", .text = "raw-error-hidden", .text_model = ava::tui::text_from_plain("model error visible")}},
          .width = 80,
          .height = 22});
  auto const visible_model_text = tui_test_support::join_visible_lines(rendered_from_model);
  expect(visible_model_text.find("model user visible") != std::string::npos && visible_model_text.find("model assistant visible") != std::string::npos &&
             visible_model_text.find("model thinking visible") != std::string::npos && visible_model_text.find("model error visible") != std::string::npos &&
             visible_model_text.find("raw-user-hidden") == std::string::npos && visible_model_text.find("raw-thinking-hidden") == std::string::npos &&
             visible_model_text.find("raw-error-hidden") == std::string::npos,
         "tui transcript renderer consumes Text models for plain transcript, thinking, and fallback assistant paths");
}
}  // namespace

void run_tui_text_model_conversion_tests()
{
  test_tui_text_model_conversions();
}

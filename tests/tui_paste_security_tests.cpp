// Exercise incomplete bracketed-paste ownership through Application's real
// Context decoder. Each fixture owns its native input endpoint on the registered
// TUI thread; no producer thread, decoder replacement or production hook is used.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/terminal/Context.h"
#include "ava/core/Application.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

namespace {

using ava::tui::runtime_input::InputPollKind;
using ava::tui::runtime_input::poll_curses_input;
using ava::tui::terminal::Key;

// The public safety boundary is one MiB of retained paste, independent of tail length.
constexpr std::size_t kPasteLimit = 1024 * 1024;
// Literal protocol delimiters used by native byte sources throughout these cases.
constexpr std::string_view kPasteStart = "\x1b[200~";
constexpr std::string_view kPasteEnd = "\x1b[201~";

// Temporarily owns the endpoint behind Application's borrowed input FILE. Only
// the registered TUI thread accesses this fixture or the decoder. Its pipe remains
// open across decoder timeouts, and fixture destruction is a real input boundary:
// retire decoder state before restoring the original descriptor and closing peers.
class NativePasteInput final
{
 public:
  // Save the process fixture's stream and install an empty pipe with a live writer;
  // no-input reads therefore exercise ncurses timeouts instead of immediate EOF.
  NativePasteInput() : stream_(ava::core::Application::instance().terminal_context().input_stream()), saved_(::dup(::fileno(stream_)))
  {
    if (!saved_)
      throw std::system_error(errno, std::generic_category(), "save paste-test Context input");
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) != 0)
      throw std::system_error(errno, std::generic_category(), "create paste-test input pipe");
    reader_.reset(descriptors[0]);
    writer_.reset(descriptors[1]);
    ava::tui::runtime_input::reset_input_decoder();
    static_cast<void>(ava::tui::terminal::Context::flushinp());
    replace(reader_.get());
  }

  // No decoder call survives this scope. Drop native replay before restoring the
  // borrowed FILE; a later fixture must never inherit this endpoint's paste state.
  ~NativePasteInput()
  {
    ava::tui::runtime_input::reset_input_decoder();
    static_cast<void>(ava::tui::terminal::Context::flushinp());
    expect(::dup2(saved_.get(), ::fileno(stream_)) >= 0, "restore paste-test Context input");
    std::clearerr(stream_);
  }

  // One fixture owns descriptor restoration and its two pipe endpoints.
  NativePasteInput(NativePasteInput const&) = delete;
  // An installed native endpoint cannot transfer between fixture lifetimes.
  NativePasteInput& operator=(NativePasteInput const&) = delete;

  // TUI thread: install complete, seekable bytes without a producer that might
  // block on the one-MiB cap. dup2 retains the temporary file after its FILE closes.
  // This changes only the native endpoint; decoder paste ownership is preserved.
  void use_file(std::string_view bytes)
  {
    ScopedTmpFile source;
    if (std::fwrite(bytes.data(), 1, bytes.size(), source.get()) != bytes.size() || std::fflush(source.get()) != 0 ||
        std::fseek(source.get(), 0, SEEK_SET) != 0)
      throw std::system_error(errno, std::generic_category(), "prepare paste-test seekable input");
    replace(::fileno(source.get()));
  }

  // TUI thread, after consuming the file completely: resume the still-owned pipe
  // without clearing decoder state or ncurses replay, so later writes are tail bytes.
  void use_pipe() { replace(reader_.get()); }

  // TUI thread: enqueue a small fragment before the next decode. The nonblocking
  // pipe makes fixture setup fail instead of hanging if a case accidentally overfills it.
  void write_bytes(std::string_view bytes) const
  {
    ssize_t written;
    do
    {
      written = ::write(writer_.get(), bytes.data(), bytes.size());
    }
    while (written < 0 && errno == EINTR);
    if (written != static_cast<ssize_t>(bytes.size()))
      throw std::system_error(written < 0 ? errno : EIO, std::generic_category(), "write paste-test input fragment");
  }

  // Internal-linkage descriptor ownership is deliberately excluded from printing.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI thread: borrow an endpoint only for dup2, preserving the registered FILE
  // identity. The borrowed descriptor and stream remain owned by their callers.
  void replace(int descriptor)
  {
    if (::dup2(descriptor, ::fileno(stream_)) < 0)
      throw std::system_error(errno, std::generic_category(), "replace paste-test Context input");
    std::clearerr(stream_);
  }

  // Application's process fixture owns this FILE and outlives descriptor restoration.
  FILE* stream_;
  // Original open-file description, restored before any fixture-owned fd closes.
  ava::core::UniqueFd saved_;
  // Native pipe read end retained while a seekable source temporarily occupies Context.
  ava::core::UniqueFd reader_;
  // Live writer prevents EOF during timeout cases; no other thread borrows this fd.
  ava::core::UniqueFd writer_;
};

// TUI thread: read exactly the supplied number of polling items. A consumed tail
// byte must be Discarded, never Input or Empty; this is the work classification
// charged by TuiSession's input budget. The bounded loop cannot hide a stuck tail.
void expect_discarded_bytes(std::size_t count, std::string_view label)
{
  std::size_t discarded = 0;
  for (std::size_t index = 0; index < count; ++index)
    if (poll_curses_input().kind == InputPollKind::Discarded)
      ++discarded;
  ava::tests::expect_eq(discarded, count, std::string(label) + ": each native byte consumes one discarded input-budget item");
}

// TUI thread: inspect an owned decoder value and require ordinary keyboard text
// after the full closing marker. No reference escapes the polling call or assertion.
void expect_keyboard_character(char character, std::string_view label)
{
  auto const input = poll_curses_input();
  expect(input.kind == InputPollKind::Input && input.input.event.key == Key::Character && !input.input.bracketed_paste &&
             input.input.text == std::string(1, character),
         std::string(label));
}

// TUI thread: a following Enter must remain a separate keyboard event, proving
// the closing marker released ownership without swallowing subsequent input.
void expect_keyboard_enter(std::string_view label)
{
  auto const input = poll_curses_input();
  expect(input.kind == InputPollKind::Input && input.input.event.key == Key::Enter && !input.input.bracketed_paste, std::string(label));
}

// TUI thread: install a cap-sized payload plus caller-owned tail and consume just
// its retained prefix. The returned flag lets a malformed fixture stop the case.
bool read_capped_prefix(NativePasteInput& input, std::string_view tail)
{
  std::string bytes(kPasteStart);
  bytes.append(kPasteLimit, 'p');
  bytes.append(tail);
  input.use_file(bytes);
  auto const prefix = poll_curses_input();
  return ava::tests::require(prefix.kind == InputPollKind::Input && prefix.input.bracketed_paste && prefix.input.text == std::string(kPasteLimit, 'p'),
                             "one-MiB paste cap returns exactly one owned paste prefix");
}

// Native cap regression: even answer-looking characters and Enter remain paste
// bytes; the terminal's full end marker alone restores fresh keyboard dispatch.
void test_capped_paste_tail_is_discarded()
{
  NativePasteInput input;
  std::string tail = "a\n";
  tail.append(64, 'a');
  tail.append(kPasteEnd);
  // Context maps CR to Enter; LF deliberately represents Shift+Enter.
  tail += "z\r";
  if (!read_capped_prefix(input, tail))
    return;
  expect_discarded_bytes(32, "first capped-paste input batch");
  expect_discarded_bytes(32, "second capped-paste input batch");
  expect_discarded_bytes(2 + kPasteEnd.size(), "remaining capped-paste tail and marker");
  expect_keyboard_character('z', "fresh character follows a capped paste's closing marker");
  expect_keyboard_enter("fresh Enter follows a capped paste's closing marker");
  expect(poll_curses_input().kind == InputPollKind::Empty, "capped-paste source is empty only after its complete tail");
}

// The open pipe supplies no further bytes until the real one-second paste read
// has returned. Prompt queue clearing and native flushing must preserve ownership
// when the delayed answer-looking tail subsequently arrives on that same pipe.
void test_timed_out_paste_survives_prompt_flush()
{
  NativePasteInput input;
  input.write_bytes(std::string(kPasteStart) + "retained");
  auto prefix = poll_curses_input();
  if (!ava::tests::require(prefix.kind == InputPollKind::Input && prefix.input.bracketed_paste && prefix.input.text == "retained",
                           "paste timeout returns its retained prefix as one paste event"))
    return;
  expect(ava::tui::runtime_input::enqueue_startup_input(std::move(prefix.input)), "timed-out prefix can be preserved by startup probing");
  ava::tui::runtime_input::clear_startup_input_queue();
  static_cast<void>(ava::tui::terminal::Context::flushinp());
  expect(ava::tui::runtime_input::startup_input_queue_size() == 0, "prompt flush removes queued paste text");
  expect(poll_curses_input().kind == InputPollKind::Empty, "temporary native emptiness preserves incomplete-paste ownership");

  input.write_bytes("a\n");
  expect_discarded_bytes(2, "delayed answer-looking paste bytes after prompt flush");
  expect(poll_curses_input().kind == InputPollKind::Empty, "drained delayed fragment still awaits the closing marker");
  input.write_bytes(std::string(kPasteEnd) + "z");
  expect_discarded_bytes(kPasteEnd.size(), "delayed closing marker");
  expect_keyboard_character('z', "keyboard input resumes after the timed-out paste actually ends");
}

// Split every interior marker boundary while paste assembly still owns its
// prefix. ESC-body parsing reaches the real timeout with a live pipe writer;
// later poll calls must remember characters already consumed by that parser.
void test_closing_marker_split_across_timeout()
{
  for (std::size_t split = 1; split < kPasteEnd.size(); ++split)
  {
    NativePasteInput input;
    input.write_bytes(std::string(kPasteStart) + "prefix" + std::string(kPasteEnd.substr(0, split)));
    auto const prefix = poll_curses_input();
    auto const label = "closing marker split at byte " + std::to_string(split);
    if (!ava::tests::require(prefix.kind == InputPollKind::Input && prefix.input.bracketed_paste && prefix.input.text.starts_with("prefix"),
                             label + " returns an owned paste after timeout"))
      continue;
    ava::tui::runtime_input::clear_startup_input_queue();
    static_cast<void>(ava::tui::terminal::Context::flushinp());
    expect(poll_curses_input().kind == InputPollKind::Empty, label + " has no native continuation yet");
    input.write_bytes(std::string(kPasteEnd.substr(split)) + "z");
    expect_discarded_bytes(kPasteEnd.size() - split, label);
    expect_keyboard_character('z', label + " releases ownership only after the marker suffix");
  }
}

// After the cap, both native emptiness and ncurses flushes can separate closing
// marker bytes across polling batches. Every incomplete prefix remains quarantined.
void test_closing_marker_split_across_discard_polls()
{
  NativePasteInput input;
  if (!read_capped_prefix(input, {}))
    return;
  input.use_pipe();
  for (std::size_t index = 0; index < kPasteEnd.size(); ++index)
  {
    input.write_bytes(kPasteEnd.substr(index, 1));
    expect_discarded_bytes(1, "closing marker fragmented across discard polls");
    expect(poll_curses_input().kind == InputPollKind::Empty, "each marker fragment ends at real native emptiness");
    ava::tui::runtime_input::clear_startup_input_queue();
    static_cast<void>(ava::tui::terminal::Context::flushinp());
  }
  input.write_bytes("z");
  expect_keyboard_character('z', "all six separately polled marker bytes release paste ownership");
}

// Malformed close markers and repeated ESC must remain discarded. The last ESC
// in a repeated run may begin the valid marker, without promoting earlier tail text.
void test_malformed_markers_and_repeated_escape()
{
  NativePasteInput input;
  std::string tail = "\x1b[201Xa\n\x1b[20\x1b\x1b";
  tail.append(kPasteEnd.substr(1));
  auto const discarded_bytes = tail.size();
  tail += "z";
  if (!read_capped_prefix(input, tail))
    return;
  expect_discarded_bytes(discarded_bytes, "malformed markers and repeated Escape in capped paste");
  expect_keyboard_character('z', "final repeated Escape can begin the actual closing marker");
  expect(poll_curses_input().kind == InputPollKind::Empty, "malformed paste markers leave no answer keys behind");
}

// An actual native-fixture boundary retires incomplete-paste state. The following
// endpoint's ordinary keys must work even when the retired paste never sent an end.
void test_decoder_reset_at_input_boundary()
{
  {
    NativePasteInput retired_input;
    if (!read_capped_prefix(retired_input, {}))
      return;
  }
  {
    NativePasteInput new_input;
    new_input.write_bytes("a\r");
    expect_keyboard_character('a', "new input fixture accepts keyboard text after decoder reset");
    expect_keyboard_enter("new input fixture accepts Enter after decoder reset");
    expect(poll_curses_input().kind == InputPollKind::Empty, "new fixture contains no retired paste replay");
  }
}

}  // namespace

// The isolated runner initializes Application's terminal and registers this TUI
// thread before calling. Each case restores its native endpoint before returning.
void run_tui_paste_security_tests()
{
  test_capped_paste_tail_is_discarded();
  test_timed_out_paste_survives_prompt_flush();
  test_closing_marker_split_across_timeout();
  test_closing_marker_split_across_discard_polls();
  test_malformed_markers_and_repeated_escape();
  test_decoder_reset_at_input_boundary();
}

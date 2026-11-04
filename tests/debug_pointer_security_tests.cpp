// Protect generated debug printers from exposing borrowed pointers and sensitive member contents.
#include "sys.h"
// clang-format off
#include "ava/debug/debug_ostream_operators.h" // Must precede the test harness's debug includes.
#include "tests/support/test_harness.h"
#include "ava/tui/Composer.h"
#include "ava/tui/SettingsDialog.h"
#include "ava/tui/SelectorDispatchResult.h"
#include "ava/tui/runtime_state_internal.h"
// clang-format on

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>

namespace ava::tests {
// Deliberately undefined: pointer formatting must not need the pointee definition.
struct DebugIncompletePointee;
} // namespace ava::tests

namespace {

// Records content-printer calls so even a hidden dereference fails the regression.
struct PrintablePointee
{
  // Updated only by the content printer during this synchronous test.
  mutable int print_count = 0;

  // Emit a recognizable secret if pointer formatting traverses this object.
  void print_on(std::ostream& output) const
  {
    ++print_count;
    output.write("pointee-secret-sentinel", 23);
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Use precisely the generated member expression, with its declared type intact.
template <typename Declared, typename T>
std::string render_member(T const& value)
{
  std::ostringstream output;
  {
    AVA_USING_OSTREAM_PRELUDE(output) << ::ava_utils::print_member<Declared>(value);
  }
  return output.str();
}

// Bypass all debug inserters when independently spelling an expected address.
std::string render_address(void const* address)
{
  std::ostringstream output;
  output.operator<<(address);
  return output.str();
}

// A generated raw pointer must emit exactly its address and never invoke print_on.
void test_printable_pointer_reports_only_address()
{
  PrintablePointee pointee;
  PrintablePointee* pointer = std::addressof(pointee);
  PrintablePointee const* const_pointee = pointer;
  PrintablePointee* const const_pointer = pointer;
  std::string const expected = render_address(pointer);

  expect(render_member<decltype(pointer)>(pointer) == expected, "generated printable pointer emits exactly its address");
  expect(render_member<decltype(const_pointee)>(const_pointee) == expected, "generated pointer to const emits exactly its address");
  expect(render_member<decltype(const_pointer)>(const_pointer) == expected, "generated top-level const pointer emits exactly its address");
  expect(pointee.print_count == 0, "generated raw pointers never invoke the pointee's content printer");
}

// Character pointers are borrowed addresses even when their storage is a valid C string.
void test_character_pointers_hide_content()
{
  char mutable_secret[] = "mutable-secret-sentinel";
  char* mutable_pointer = mutable_secret;
  char const* const_pointer = "constant-secret-sentinel";
  std::string const mutable_rendered = render_member<decltype(mutable_pointer)>(mutable_pointer);
  std::string const const_rendered = render_member<decltype(const_pointer)>(const_pointer);

  expect(mutable_rendered == render_address(mutable_pointer), "generated char pointer emits exactly its address");
  expect(const_rendered == render_address(const_pointer), "generated const char pointer emits exactly its address");
  expect(mutable_rendered.find(mutable_secret) == std::string::npos, "generated char pointer hides its secret sentinel");
  expect(const_rendered.find(const_pointer) == std::string::npos, "generated const char pointer hides its secret sentinel");
}

// Volatile qualification must not turn an address into a boolean or require a read.
void test_volatile_pointers_report_addresses()
{
  int volatile volatile_value = 31;
  int const volatile const_volatile_value = 47;
  int volatile* volatile_pointer = std::addressof(volatile_value);
  int const volatile* const_volatile_pointer = std::addressof(const_volatile_value);
  void const* volatile_address = const_cast<void const*>(static_cast<void const volatile*>(volatile_pointer));
  void const* const_volatile_address = const_cast<void const*>(static_cast<void const volatile*>(const_volatile_pointer));

  expect(render_member<decltype(volatile_pointer)>(volatile_pointer) == render_address(volatile_address),
         "generated volatile object pointer emits exactly its address");
  expect(render_member<decltype(const_volatile_pointer)>(const_volatile_pointer) == render_address(const_volatile_address),
         "generated const volatile object pointer emits exactly its address");
}

// Null pointers retain a readable marker, including pointers to incomplete types.
void test_null_pointers_report_nullptr()
{
  PrintablePointee* printable = nullptr;
  char* characters = nullptr;
  char const* const_characters = nullptr;
  ava::tests::DebugIncompletePointee* incomplete = nullptr;
  int (*array)[3] = nullptr;
  int (*function)(int) = nullptr;
  std::nullptr_t null_value = nullptr;

  expect(render_member<decltype(printable)>(printable) == "nullptr", "generated null printable pointer uses the nullptr marker");
  expect(render_member<decltype(characters)>(characters) == "nullptr", "generated null char pointer uses the nullptr marker");
  expect(render_member<decltype(const_characters)>(const_characters) == "nullptr", "generated null const char pointer uses the nullptr marker");
  expect(render_member<decltype(incomplete)>(incomplete) == "nullptr", "generated incomplete pointee needs no definition");
  expect(render_member<decltype(array)>(array) == "nullptr", "generated null pointer to array uses the nullptr marker");
  expect(render_member<decltype(function)>(function) == "nullptr", "generated null function pointer uses the nullptr marker");
  expect(render_member<decltype(null_value)>(null_value) == "nullptr", "generated nullptr value retains its existing marker");
}

// Provide an ordinary function address without executing the callback during printing.
int callback_target(int value)
{
  return value;
}

// Arrays and functions require address rendering without treating their targets as values.
void test_array_and_function_pointer_addresses()
{
  int values[] = {13, 17, 19};
  int (*array)[3] = std::addressof(values);
  int (*function)(int) = &callback_target;

  expect(render_member<decltype(array)>(array) == render_address(array), "generated pointer to array emits exactly its address");
  // AVA's POSIX targets support conversion between function pointers and void pointers.
  expect(render_member<decltype(function)>(function) == render_address(reinterpret_cast<void const*>(function)),
         "generated ordinary function pointer emits exactly its address");
}

// Owned strings retain contents while borrowed members retain reference identity.
void test_owned_string_and_reference_semantics()
{
  std::string owned = "owned debug value";
  std::string& mutable_reference = owned;
  std::string const& const_reference = owned;
  std::string const mutable_rendered = render_member<decltype(mutable_reference)>(mutable_reference);
  std::string const const_rendered = render_member<decltype(const_reference)>(const_reference);
  std::string const reference_suffix = '@' + render_address(std::addressof(owned));

  expect(render_member<decltype(owned)>(owned) == "\"owned debug value\"", "generated owned string keeps its quoted contents");
  expect(mutable_rendered.ends_with(reference_suffix) && mutable_rendered.find(owned) == std::string::npos,
         "generated mutable reference keeps referent identity and hides content");
  expect(const_rendered.ends_with(reference_suffix) && const_rendered.find(owned) == std::string::npos,
         "generated const reference keeps referent identity and hides content");

  PrintablePointee pointee;
  PrintablePointee* pointer = std::addressof(pointee);
  PrintablePointee*& pointer_reference = pointer;
  std::string const pointer_rendered = render_member<decltype(pointer_reference)>(pointer_reference);
  expect(pointer_rendered.ends_with('@' + render_address(std::addressof(pointer))), "generated reference to a pointer keeps the pointer object's identity");
  expect(pointee.print_count == 0, "generated reference to a pointer does not inspect the ultimate pointee");
}

// NoLogString annotations must omit sensitive fields while preserving ordinary metadata.
void test_archive_action_hides_session_id()
{
  std::string const secret = "archive-session-secret-sentinel";
  ava::tui::PendingSessionArchiveAction const action{.session_id = secret, .archive = false};
  std::string const rendered = render_member<decltype(action)>(action);

  expect(rendered.find(secret) == std::string::npos, "generated archive action hides its sensitive session ID");
  expect(rendered.find("session_id") == std::string::npos, "generated archive action omits the entire NoLogString member");
  expect(rendered.find("archive:false") != std::string::npos, "generated archive action preserves its selected archive flag");
}

// Editor and Settings results keep useful flags while omitting every owned content string.
void test_editor_results_hide_content()
{
  std::string const secret = "private-editor-feedback-sentinel";
  ava::tui::ComposerInputResult const result{.handled = true, .status = secret, .beep = true};
  auto const rendered = render_member<decltype(result)>(result);
  expect(rendered.find(secret) == std::string::npos && rendered.find("status:") == std::string::npos,
         "generated Composer input feedback omits the entire annotated string");
  expect(
      rendered.find("handled:true") != std::string::npos && rendered.find("beep:true") != std::string::npos && rendered.find("request:") != std::string::npos,
      "generated Composer input result retains its non-content routing metadata");

  ava::tui::DraftCommand const command{.command = "private-command-sentinel", .status = secret};
  auto const draft = render_member<decltype(command)>(command);
  expect(draft.find("private-command-sentinel") == std::string::npos && draft.find(secret) == std::string::npos &&
             draft.find("command:") == std::string::npos && draft.find("status:") == std::string::npos,
         "generated draft command omits both command text and feedback even when populated");
}

// Each selector payload hides its strings; the saved-view payload exposes only its row index.
void test_selector_results_hide_content()
{
  using namespace ava::tui;
  std::string const secret = "private-selector-payload-sentinel";
  SelectorDraftCompletion const completion{.command = secret, .status = secret};
  SelectorDraftCommand const command{.command = secret};
  SelectorOpenedSession const opened{.feedback = secret};
  SelectorSessionError const error{.feedback = secret};
  SelectListView previous;
  previous.title = secret;
  SelectorParentSummary const summary{.selected_source = secret, .previous_view = std::move(previous), .selected_index = 7};
  auto const logged_summary = render_member<decltype(summary)>(summary);
  expect(render_member<decltype(completion)>(completion).find(secret) == std::string::npos &&
             render_member<decltype(command)>(command).find(secret) == std::string::npos &&
             render_member<decltype(opened)>(opened).find(secret) == std::string::npos &&
             render_member<decltype(error)>(error).find(secret) == std::string::npos && logged_summary.find(secret) == std::string::npos,
         "selector result printers omit command, feedback, source identity and saved view text");
  expect(logged_summary.find("selected_index:7") != std::string::npos, "summary diagnostic retains its non-content row position");
  SelectorDispatchResult const result = summary;
  expect(render_member<decltype(result)>(result).find(secret) == std::string::npos, "printing the selector result variant cannot expose its owned payload");
}

} // namespace

// Exercise generated raw-pointer privacy and preserve neighboring value/reference behavior.
void run_debug_pointer_security_tests()
{
  test_printable_pointer_reports_only_address();
  test_character_pointers_hide_content();
  test_volatile_pointers_report_addresses();
  test_null_pointers_report_nullptr();
  test_array_and_function_pointer_addresses();
  test_owned_string_and_reference_semantics();
  test_archive_action_hides_session_id();
  test_editor_results_hide_content();
  test_selector_results_hide_content();
}

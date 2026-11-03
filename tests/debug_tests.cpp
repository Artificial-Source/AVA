#include "sys.h"
// clang-format off
#include "ava/debug/debug_ostream_operators.h"          // This header must be included before test_harness.h

#include "tests/support/test_harness.h"
#include "ava/app/runtime/RunOptions.h"
// clang-format on

#include <array>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <boost/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include "debug.h"

// A type in namespace ava::session that owns a print_on method, used to exercise
// printing of an owning pointer (boost::intrusive_ptr) and an optional of one.
namespace ava::session {
struct Foo : boost::intrusive_ref_counter<Foo, boost::thread_unsafe_counter>
{
  void print_on(std::ostream& os) const { os << "{Foo}"; }
};
} // namespace ava::session

namespace {

template <typename T>
concept DebugStreamInsertable = requires(std::ostream& output, T const& value) { output << value; };

static_assert(!DebugStreamInsertable<ava::app::runtime::RunOptions>,
              "RunOptions contains credentials and authority callbacks and must remain unavailable to debug streams");

// An enum with an ADL-findable to_string; after LIBCWD_USING_OSTREAM_PRELUDE it
// should render as that string.
namespace test_adl {
enum class Color
{
  Red,
  Green,
  Blue
};
// Give the debug formatter an ADL customization whose uppercase output distinguishes it from fallback formatting.
std::string to_string(Color color)
{
  switch (color)
  {
    // Use uppercase to distinguish this function from utils::to_string.
    case Color::Red:
      return "RED";
    case Color::Green:
      return "GREEN";
    case Color::Blue:
      return "BLUE";
  }
  return "Unknown";
}
} // namespace test_adl

// An enum WITHOUT a to_string. This is the case the prelude still needs to grow
// support for (scoped enums cannot fall back to their underlying integer).
enum class Plain
{
  Alpha,
  Beta,
  Gamma
};

// Helper that builds the debug rendering of every supported value kind in a
// single stringstream, using LIBCWD_USING_OSTREAM_PRELUDE exactly the way a
// print_on/print_members body would.
std::string render_debug_values()
{
  std::stringstream ss;

  std::string const string_value = "hello";
  bool const bool_value = true;
  int const int_value = 42;
  test_adl::Color const color_value = test_adl::Color::Green;
  Plain const plain_value = Plain::Beta;
  std::mutex mutex_value;
  std::optional<bool> const optional_bool = true;
  std::optional<std::string> const optional_string_value = std::string("present");
  std::optional<std::string> const optional_string_empty;
  ava::session::Foo const foo;
  boost::intrusive_ptr<ava::session::Foo> const foo_ptr(new ava::session::Foo);
  std::optional<boost::intrusive_ptr<ava::session::Foo>> const optional_foo(foo_ptr);
  std::optional<boost::intrusive_ptr<ava::session::Foo>> const optional_foo_empty;
  std::vector<bool> vector_bool = {true, false};
  std::vector<std::string> vector_strings = {"one", "two"};
  std::shared_ptr<ava::session::Foo> shared_foo_ptr(new ava::session::Foo);
  std::shared_ptr<ava::session::Foo> shared_foo_ptr_empty;

  {
    AVA_USING_OSTREAM_PRELUDE(ss) << __write__("string_literal:") << "literal" << __write__("\nstd::string:") << string_value << __write__("\nbool:")
                                  << bool_value << __write__("\nint:") << int_value << __write__("\nenum_to_string:") << color_value
                                  << __write__("\nenum_plain:") << plain_value << __write__("\nmutex:") << mutex_value << __write__("\noptional_string:")
                                  << optional_string_value << __write__("\noptional_string_empty:") << optional_string_empty << __write__("\nFoo:") << foo
                                  << __write__("\nintrusive_ptr:") << foo_ptr << __write__("\noptional_intrusive_ptr:") << optional_foo
                                  << __write__("\noptional_intrusive_ptr_empty:") << optional_foo_empty << __write__("\nvector_with_bool:") << vector_bool
                                  << __write__("\nvector_with_strings:") << vector_strings << __write__("\noptional_bool:") << optional_bool
                                  << __write__("\nshared_foo_ptr:") << shared_foo_ptr << __write__("\nshared_foo_ptr_empty:") << shared_foo_ptr_empty << '\n';
  }

  std::cout << "Result: " << ss.str();
  return ss.str();
}

// Protect raw string-literal formatting while owned strings remain quoted.
void test_string_literal_renders_raw()
{
  std::stringstream ss;
  {
    ss << "<prefix>";
    AVA_USING_OSTREAM_PRELUDE(ss) << __write__("literal:") << "literal";
  }
  expect(ss.str() == "<prefix>literal:\"literal\"", "prefix renders raw, string literal is quoted");
}

void test_std_string_is_quoted()
{
  std::stringstream ss;
  std::string const value = "hello";
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << value;
  }
  expect(ss.str() == "\"hello\"", "std::string renders double-quoted");
}

void test_maxlen_truncates_long_string()
{
  // A 150-char string under maxlen(100) loses 68 chars from the middle: the
  // kept 82 chars (41 + 41) plus the 18-char marker "\"...(68 chars)...\"" fill
  // the 100-char limit exactly.
  std::stringstream ss;
  std::string const s150(150, 'a');
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << maxlen(100) << s150;
  }
  std::string const expected = "\"" + std::string(41, 'a') + "\"...(68 chars)...\"" + std::string(41, 'a') + "\"";
  if (ss.str() != expected)
    std::cout << "ss.str() = \"" << ss.str() << "\"." << std::endl;
  expect(ss.str() == expected, "long string is cut in the middle with removed-count marker");
}

void test_maxlen_short_string_prints_full()
{
  std::stringstream ss;
  std::string const value = "hello";
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << maxlen(100) << value;
  }
  expect(ss.str() == "\"hello\"", "string shorter than maxlen prints in full");
}

void test_maxlen_under_20_removed_prints_full()
{
  // A 101-char string under maxlen(100) would remove only 19 chars, which is
  // below the 20-char threshold, so the full string is printed instead.
  std::stringstream ss;
  std::string const s101(101, 'a');
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << maxlen(100) << s101;
  }
  expect(ss.str() == "\"" + std::string(101, 'a') + "\"", "string that would remove <20 chars prints in full");
}

// Protect the exact truncation threshold so boundary-length values cannot silently bypass the output limit.
void test_maxlen_exactly_20_removed_truncates()
{
  // A 102-char string under maxlen(100) removes exactly 20 chars, which meets the threshold, so the middle is cut.
  std::stringstream ss;
  std::string const s102(102, 'a');
  int const ml = 100;
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << maxlen(ml) << s102;
  }
  int part_len = (ml - 18) / 2;
  std::string const expected =
      "\"" + std::string(part_len, 'a') + "\"...(" + std::to_string(102 - 2 * part_len) + " chars)...\"" + std::string(part_len, 'a') + "\"";
  expect(ss.str() == expected, "string that removes exactly 20 chars is truncated");
}

void test_maxlen_three_digit_removal()
{
  // A 1000-char string under maxlen(100) removes 919 chars; the 3-digit count
  // widens the marker to 19 chars, leaving 81 kept (40 + 41).
  std::stringstream ss;
  std::string const s1000(1000, 'a');
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << maxlen(100) << s1000;
  }
  std::string const expected = "\"" + std::string(40, 'a') + "\"...(919 chars)...\"" + std::string(41, 'a') + "\"";
  if (ss.str() != expected)
    std::cout << "ss.str() = \"" << ss.str() << "\"." << std::endl;
  expect(ss.str() == expected, "very long string truncates with a 3-digit removed count");
}

// Protect expression-local debug limits from leaking into the next log operation.
void test_maxlen_is_scoped_to_expression()
{
  // maxlen() derives from utils::iomanip::Unsticky: the limit is restored when
  // the manipulator temporary is destroyed at the end of the statement, so a
  // string printed in a later expression is not truncated.
  std::stringstream ss;
  std::string const s50(config::ava_debug_maxlen_c, 'a');
  int const ml = 30;
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << maxlen(ml) << s50;
  }
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << s50;
  }
  int const part_len = (ml - 18) / 2;
  std::string const truncated = "\"" + std::string(part_len, 'a') + "\"...(" + std::to_string(config::ava_debug_maxlen_c - 2 * part_len) + " chars)...\"" +
                                std::string(part_len, 'a') + "\"";
  std::string const full = "\"" + s50 + "\"";
  expect(ss.str() == truncated + full, "maxlen is scoped to its expression (Unsticky restores stream state)");
}

void test_maxlen_applies_to_whole_expression()
{
  // Both strings streamed after maxlen() in the same expression are truncated.
  std::stringstream ss;
  std::string const s150(150, 'a');
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << maxlen(100) << s150 << s150;
  }
  std::string const truncated = "\"" + std::string(41, 'a') + "\"...(68 chars)...\"" + std::string(41, 'a') + "\"";
  expect(ss.str() == truncated + truncated, "maxlen applies to every string in the expression");
}

void test_bool_renders_boolalpha()
{
  // NOTE: boolalpha is not yet handled by the prelude operators. This assertion
  // encodes the desired contract; tighten the project operators to make it pass.
  std::stringstream ss;
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << true;
  }
  expect(ss.str() == "true", "bool renders through std::boolalpha");
}

void test_int_renders_value()
{
  std::stringstream ss;
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << 42;
  }
  expect(ss.str() == "42", "int renders its value");
}

void test_enum_with_to_string_renders_name()
{
  std::stringstream ss;
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << test_adl::Color::Green;
  }
  expect(ss.str() == "GREEN", "enum with to_string renders using that to_string");
}

void test_enum_without_to_string_renders()
{
  // NOTE: scoped enums without to_string are not yet printable through the
  // prelude; this is the gap to fill. The expectation is intentionally lenient.
  std::stringstream ss;
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << Plain::Beta;
  }
  expect(ss.str() == "Beta", "enum without to_string renders using enchantum");
}

void test_mutex_renders_marker()
{
  std::stringstream ss;
  std::mutex m;
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << m;
  }
  expect(ss.str() == "$mutex$", "std::mutex renders its marker");
}

void test_optional_string_renders_value_or_marker()
{
  {
    std::stringstream ss;
    std::optional<std::string> const value = std::string("present");
    {
      AVA_USING_OSTREAM_PRELUDE(ss) << value;
    }
    if (ss.str() != "\"present\"")
      std::cout << "ss.str() = \"" << ss.str() << "\"." << std::endl;
    expect(ss.str() == "\"present\"", "engaged optional<string> renders the quoted value");
  }
  {
    std::stringstream ss;
    std::optional<std::string> const empty;
    {
      AVA_USING_OSTREAM_PRELUDE(ss) << empty;
    }
    expect(ss.str() == "$no_value$", "disengaged optional renders the marker");
  }
}

void test_optional_intrusive_ptr_renders()
{
  std::stringstream ss;
  boost::intrusive_ptr<ava::session::Foo> const foo(new ava::session::Foo);
  std::optional<boost::intrusive_ptr<ava::session::Foo>> const value(foo);
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << value;
  }
  // The engaged case should dereference through print_pointer and show {Foo}.
  expect(ss.str().find("{Foo}") != std::string::npos, "engaged optional<intrusive_ptr> dereferences Foo");

  std::stringstream ss_empty;
  std::optional<boost::intrusive_ptr<ava::session::Foo>> const empty;
  {
    AVA_USING_OSTREAM_PRELUDE(ss_empty) << empty;
  }
  expect(ss_empty.str() == "$no_value$", "disengaged optional<intrusive_ptr> renders the marker");
}

// Protect readable boolean elements despite vector<bool> proxy references.
void test_vector_bool_renders_boolalpha_list()
{
  std::stringstream ss;
  std::vector<bool> const vector_bool = {true, false};
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << vector_bool;
  }
  expect(ss.str() == "{true, false}", "vector of bools renders elements with boolalpha");
}

// Protect element quoting when the debug printer formats a string collection.
void test_vector_string_renders_quoted_list()
{
  std::stringstream ss;
  std::vector<std::string> const vector_string = {"one", "two"};
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << vector_string;
  }
  // Should render each element quoted, but not the ", " seperator.
  expect(ss.str() == "{\"one\", \"two\"}", "vector of strings renders elements quoted");
}

void test_span_const_char_pointer_renders_quoted_list()
{
  // BuiltinGenericModelSpec::reasoning_levels is std::span<char const* const>; Debug
  // print_members streams that field, so the span inserter must compile and quote
  // each C string the same way as other debug element formatting.
  std::array<char const*, 2> const levels = {"low", "high"};
  std::span<char const* const> const reasoning_levels(levels);

  std::stringstream ss;
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << reasoning_levels;
  }
  expect(ss.str() == "{\"low\", \"high\"}", "span of C strings renders quoted elements with container braces");

  std::span<char const* const> const empty_levels;
  std::stringstream ss_empty;
  {
    AVA_USING_OSTREAM_PRELUDE(ss_empty) << empty_levels;
  }
  expect(ss_empty.str() == "{}", "empty span renders empty container braces");
}

void test_optional_bool_renders_with_boolalpha()
{
  std::stringstream ss;
  std::optional<bool> const optional_bool = true;
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << optional_bool;
  }
  expect(ss.str() == "true", "engaged optional<bool> renders with boolalpha");
}

void test_shared_ptr_uses_print_pointer()
{
  std::stringstream ss;
  std::shared_ptr<ava::session::Foo> const shared_foo_ptr(new ava::session::Foo);
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << shared_foo_ptr;
  }
  expect(ss.str().find("&{Foo}@0x") != std::string::npos, "shared_ptr<Foo> dereferences Foo and shows ptr value");

  std::stringstream ss_empty;
  std::shared_ptr<ava::session::Foo> const shared_foo_ptr_empty;
  {
    AVA_USING_OSTREAM_PRELUDE(ss_empty) << shared_foo_ptr_empty;
  }
  expect(ss_empty.str() == "nullptr", "Empty shared_ptr<Foo> renders as nullptr");
}

void test_unique_ptr_uses_print_pointer()
{
  std::stringstream ss;
  std::unique_ptr<ava::session::Foo> const unique_foo_ptr(new ava::session::Foo);
  {
    AVA_USING_OSTREAM_PRELUDE(ss) << unique_foo_ptr;
  }
  expect(ss.str().find("&{Foo}@0x") != std::string::npos, "unique_ptr<Foo> dereferences Foo and shows ptr value");

  std::stringstream ss_empty;
  std::unique_ptr<ava::session::Foo> const unique_foo_ptr_empty;
  {
    AVA_USING_OSTREAM_PRELUDE(ss_empty) << unique_foo_ptr_empty;
  }
  expect(ss_empty.str() == "nullptr", "Empty unique_ptr<Foo> renders as nullptr");

  std::stringstream ss_vector;
  std::vector<std::unique_ptr<ava::session::Foo>> unique_foo_ptrs;
  unique_foo_ptrs.push_back(std::make_unique<ava::session::Foo>());
  {
    AVA_USING_OSTREAM_PRELUDE(ss_vector) << unique_foo_ptrs;
  }
  expect(ss_vector.str().find("{&{Foo}@0x") == 0, "vector<unique_ptr<Foo>> uses the debug pointer inserter for its elements");
}

// Generated reference printing must not expose either mutable or const borrowed content.
void test_generated_reference_members_do_not_print_content()
{
  std::string value = "borrowed private content";
  std::string& mutable_reference = value;
  std::string const& const_reference = value;
  std::stringstream borrowed;
  {
    AVA_USING_OSTREAM_PRELUDE(borrowed) << ::ava_utils::print_member<decltype(mutable_reference)>(mutable_reference)
                                        << ::ava_utils::print_member<decltype(const_reference)>(const_reference);
  }
  expect(borrowed.str().find(value) == std::string::npos && borrowed.str().find('@') != std::string::npos,
         "generated mutable and const references report addresses without borrowed content");
  std::stringstream owned;
  {
    AVA_USING_OSTREAM_PRELUDE(owned) << ::ava_utils::print_member<decltype(value)>(value);
  }
  expect(owned.str().find(value) != std::string::npos, "generated owned values retain their existing printer");
}

// Weak printing must neither traverse content nor retain the target after its owner releases it.
void test_weak_ptr_prints_only_lifetime()
{
  auto owner = std::make_shared<std::string>("private weak target");
  std::weak_ptr<std::string> weak = owner;
  std::stringstream live;
  {
    AVA_USING_OSTREAM_PRELUDE(live) << weak;
  }
  expect(live.str() == "$weak_ptr:live$" && owner.use_count() == 1, "weak debug printing reports only live metadata");
  owner.reset();
  std::stringstream expired;
  {
    AVA_USING_OSTREAM_PRELUDE(expired) << weak;
  }
  expect(expired.str() == "$weak_ptr:expired$", "weak debug printing does not keep its target alive");
}

void test_full_render_smoke()
{
  // Exercises every requested type in one stream; mainly a compile-and-run smoke.
  std::string const rendered = render_debug_values();
  expect(!rendered.empty(), "combined render produces output");
}

} // namespace

// Exercise owned-value formatting and privacy of borrowed references and weak capabilities.
void run_debug_tests()
{
  test_string_literal_renders_raw();
  test_std_string_is_quoted();
  test_maxlen_truncates_long_string();
  test_maxlen_short_string_prints_full();
  test_maxlen_under_20_removed_prints_full();
  test_maxlen_exactly_20_removed_truncates();
  test_maxlen_three_digit_removal();
  test_maxlen_is_scoped_to_expression();
  test_maxlen_applies_to_whole_expression();
  test_bool_renders_boolalpha();
  test_int_renders_value();
  test_enum_with_to_string_renders_name();
  test_enum_without_to_string_renders();
  test_mutex_renders_marker();
  test_optional_string_renders_value_or_marker();
  test_optional_intrusive_ptr_renders();
  test_vector_bool_renders_boolalpha_list();
  test_vector_string_renders_quoted_list();
  test_span_const_char_pointer_renders_quoted_list();
  test_optional_bool_renders_with_boolalpha();
  test_shared_ptr_uses_print_pointer();
  test_unique_ptr_uses_print_pointer();
  test_generated_reference_members_do_not_print_content();
  test_weak_ptr_prints_only_lifetime();
  test_full_render_smoke();
}

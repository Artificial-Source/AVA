#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/core/utf8.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace {

// Build a byte string from numeric values so malformed UTF-8 fixtures remain independent of source-file encoding.
std::string bytes(std::initializer_list<unsigned int> values)
{
  std::string result;
  result.reserve(values.size());
  for (auto const value : values)
    result.push_back(static_cast<char>(value));
  return result;
}

// Assert that decoding `text` at `index` produces `scalar` and consumes exactly `length` bytes.
void expect_scalar(std::string_view text, std::size_t index, std::uint32_t scalar, std::size_t length, std::string_view description)
{
  auto const decoded = ava::core::decode_utf8_scalar(text, index);
  expect(decoded && decoded->scalar == scalar && decoded->length == length, std::string(description));
}

// Assert that an invalid scalar starting at `index` in `text` is rejected without consuming input.
void expect_invalid(std::string_view text, std::size_t index, std::string_view description)
{
  expect(!ava::core::decode_utf8_scalar(text, index), std::string(description));
}

// Cover permitted lead-byte widths and invalid lead ranges without relying on a complete sequence.
void test_utf8_expected_width()
{
  expect(ava::core::utf8_expected_width('A') == 1 && ava::core::utf8_expected_width(0x7fU) == 1, "UTF-8 ASCII leads have width one");
  expect(ava::core::utf8_expected_width(0xc2U) == 2 && ava::core::utf8_expected_width(0xdfU) == 2, "UTF-8 two-byte lead boundaries are recognized");
  expect(ava::core::utf8_expected_width(0xe0U) == 3 && ava::core::utf8_expected_width(0xefU) == 3, "UTF-8 three-byte lead boundaries are recognized");
  expect(ava::core::utf8_expected_width(0xf0U) == 4 && ava::core::utf8_expected_width(0xf4U) == 4, "UTF-8 four-byte lead boundaries are recognized");
  expect(ava::core::utf8_expected_width(0x80U) == 0 && ava::core::utf8_expected_width(0xbfU) == 0 && ava::core::utf8_expected_width(0xc0U) == 0 &&
             ava::core::utf8_expected_width(0xc1U) == 0 && ava::core::utf8_expected_width(0xf5U) == 0 && ava::core::utf8_expected_width(0xffU) == 0,
         "UTF-8 continuation and forbidden lead bytes have no expected width");
}

// Cover scalar boundaries across all four encoded widths, including nonzero input offsets.
void test_utf8_valid_scalars_and_boundaries()
{
  expect_scalar("A", 0, 0x41U, 1, "UTF-8 decodes ASCII");
  expect_scalar(bytes({0x00}), 0, 0x00U, 1, "UTF-8 decodes the minimum ASCII scalar");
  expect_scalar(bytes({0x7f}), 0, 0x7fU, 1, "UTF-8 decodes the maximum ASCII scalar");
  auto const embedded = std::string("x") + bytes({0xc2, 0xa2}) + "y";
  expect_scalar(embedded, 1, 0xa2U, 2, "UTF-8 decodes a scalar at a nonzero index");
  expect_scalar(bytes({0xc2, 0x80}), 0, 0x80U, 2, "UTF-8 decodes the minimum two-byte scalar");
  expect_scalar(bytes({0xdf, 0xbf}), 0, 0x7ffU, 2, "UTF-8 decodes the maximum two-byte scalar");
  expect_scalar(bytes({0xe0, 0xa0, 0x80}), 0, 0x800U, 3, "UTF-8 decodes the minimum three-byte scalar");
  expect_scalar(bytes({0xed, 0x9f, 0xbf}), 0, 0xd7ffU, 3, "UTF-8 decodes the scalar immediately before the surrogate range");
  expect_scalar(bytes({0xee, 0x80, 0x80}), 0, 0xe000U, 3, "UTF-8 decodes the scalar immediately after the surrogate range");
  expect_scalar(bytes({0xef, 0xbf, 0xbf}), 0, 0xffffU, 3, "UTF-8 decodes the maximum three-byte scalar");
  expect_scalar(bytes({0xf0, 0x90, 0x80, 0x80}), 0, 0x10000U, 4, "UTF-8 decodes the minimum four-byte scalar");
  expect_scalar(bytes({0xf4, 0x8f, 0xbf, 0xbf}), 0, 0x10ffffU, 4, "UTF-8 decodes U+10FFFF");
}

// Reject missing bytes, malformed continuations, overlong encodings, surrogates and out-of-range values.
void test_utf8_invalid_sequences()
{
  expect_invalid({}, 0, "UTF-8 rejects an index in empty input");
  expect_invalid("a", 1, "UTF-8 rejects an index at end of input");
  expect_invalid(bytes({0xc2}), 0, "UTF-8 rejects a truncated two-byte sequence");
  expect_invalid(bytes({0xe2, 0x82}), 0, "UTF-8 rejects a truncated three-byte sequence");
  expect_invalid(bytes({0xf0, 0x9f, 0x92}), 0, "UTF-8 rejects a truncated four-byte sequence");
  expect_invalid(bytes({0x80}), 0, "UTF-8 rejects a continuation as a lead");
  expect_invalid(bytes({0xc0, 0x80}), 0, "UTF-8 rejects an invalid C0 lead");
  expect_invalid(bytes({0xc2, 0x20}), 0, "UTF-8 rejects a malformed continuation");
  expect_invalid(bytes({0xe2, 0x82, 0x20}), 0, "UTF-8 rejects a malformed later continuation");
  expect_invalid(bytes({0xe0, 0x80, 0x80}), 0, "UTF-8 rejects a three-byte overlong encoding");
  expect_invalid(bytes({0xf0, 0x80, 0x80, 0x80}), 0, "UTF-8 rejects a four-byte overlong encoding");
  expect_invalid(bytes({0xed, 0xa0, 0x80}), 0, "UTF-8 rejects the first surrogate");
  expect_invalid(bytes({0xed, 0xbf, 0xbf}), 0, "UTF-8 rejects the last surrogate");
  expect_invalid(bytes({0xf4, 0x90, 0x80, 0x80}), 0, "UTF-8 rejects values above U+10FFFF");
  expect_invalid(bytes({0xf5, 0x80, 0x80, 0x80}), 0, "UTF-8 rejects an out-of-range lead");
}

}  // namespace

// Run the strict scalar-decoding and lead-width contract regressions.
void run_core_utf8_tests()
{
  test_utf8_expected_width();
  test_utf8_valid_scalars_and_boundaries();
  test_utf8_invalid_sequences();
}

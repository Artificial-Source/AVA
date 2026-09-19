#include "sys.h"
#include "terminal_test_support.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <system_error>
#include <unistd.h>
#include "debug.h"

namespace {

// The first sixteen xterm palette colors as packed 8-bit sRGB values.
constexpr std::array<unsigned int, 16> xterm_16color = {0x000000, 0xb21818, 0x18b218, 0xb26818, 0x1818b2, 0xb218b2, 0x18b2b2, 0xb2b2b2,
                                                        0x686868, 0xff5454, 0x54ff54, 0xffff54, 0x5454ff, 0xff54ff, 0x54ffff, 0xffffff};

constexpr std::array<unsigned int, 6> xterm_cube_levels = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};

} // namespace

void reset_output_file(FILE* file)
{
  if (std::fflush(file) != 0)
    throw std::system_error(errno, std::generic_category(), "flush test output");

  if (::ftruncate(::fileno(file), 0) != 0)
    throw std::system_error(errno, std::generic_category(), "truncate test output");

  if (std::fseek(file, 0, SEEK_SET) != 0)
    throw std::system_error(errno, std::generic_category(), "rewind test output");

  std::clearerr(file);
}

// Write the ordered OSC 4 replies for a fixed xterm-style 16- or 256-color palette and its subsequent write-test probes to `file`.
//
// The caller must pass a non-null writable stream and either 16 or 256 colors. Replies use xterm's four-hex-digit channel form and
// BEL terminators. Write-test replies report each original color unchanged, and the function does not flush the stream.
void write_OSC4_reply(FILE* file, int number_of_colors)
{
  // Pass the writable FILE that represents terminal input; create one with tmpfile when constructing a terminal test fixture.
  ASSERT(file != nullptr);
  // Request exactly an xterm 16- or 256-color response; add a separate generator before using this helper for another palette shape.
  ASSERT(number_of_colors == 16 || number_of_colors == 256);

  auto const write_reply = [file](int index, unsigned int red, unsigned int green, unsigned int blue) {
    static_cast<void>(std::fprintf(file, "\x1b]4;%d;rgb:%02x%02x/%02x%02x/%02x%02x\a", index, red, red, green, green, blue, blue));
  };

  for (int index = 0; index != 16; ++index)
  {
    unsigned int const rgb = xterm_16color[static_cast<std::size_t>(index)];
    write_reply(index, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
  }

  if (number_of_colors == 256)
  {
    int index = 16;
    for (unsigned int red : xterm_cube_levels)
    {
      for (unsigned int green : xterm_cube_levels)
      {
        for (unsigned int blue : xterm_cube_levels)
          write_reply(index++, red, green, blue);
      }
    }

    for (unsigned int gray = 8; gray <= 238; gray += 10)
      write_reply(index++, gray, gray, gray);

    // The cube and grayscale loops must emit exactly entries 16 through 255.
    ASSERT(index == 256);
  }

  // Write the replies to the palette write attempt probes.
  for (int n = 3; (1 << n) - 1 < number_of_colors; ++n)
  {
    int index = (1 << n) - 1;
    if (index < 16)
    {
      unsigned int const rgb = xterm_16color[static_cast<std::size_t>(index)];
      write_reply(index, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
    }
    else if (index < 232)
    {
      int const cube_index = index - 16;
      unsigned int const red = xterm_cube_levels[static_cast<std::size_t>(cube_index / 36)];
      unsigned int const green = xterm_cube_levels[static_cast<std::size_t>((cube_index / 6) % 6)];
      unsigned int const blue = xterm_cube_levels[static_cast<std::size_t>(cube_index % 6)];
      write_reply(index, red, green, blue);
    }
    else
    {
      unsigned int const gray = static_cast<unsigned int>(8 + 10 * (index - 232));
      write_reply(index, gray, gray, gray);
    }
  }
}

void write_KeyboardInputMode_reply(FILE* file, SupportedMode mode)
{
  static constexpr char const* kKittyProtocolReply = "\x1b[?1u";
  static constexpr char const* kDeviceAttributesReply = "\x1b[?1;2c";
  static constexpr char const* kModifyOtherKeysReply = "\x1b[>4;2m";

  // The Kitty query reply, when supported, is followed by its device-attributes phase fence.
  if (mode == SupportedMode::KittyProtocol)
    std::fprintf(file, kKittyProtocolReply);
  std::fprintf(file, kDeviceAttributesReply);

  if (mode == SupportedMode::KittyProtocol)
    return;     // No other probes follow.

  // A fallback XTMODKEYS query reply, when supported, is followed by its own device-attributes phase fence.
  if (mode == SupportedMode::ModifyOtherKeys)
    std::fprintf(file, kModifyOtherKeysReply);
  std::fprintf(file, kDeviceAttributesReply);

  // No other probes follow.
}

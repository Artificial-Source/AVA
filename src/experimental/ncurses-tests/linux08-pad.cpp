#include "sys.h"
#include "terminal/Context.h"
#include "terminal/Pad.h"
#include "ava/core/Application.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string_view>
#include "debug.h"

class Application : public ava::core::Application
{
 public:
  [[nodiscard]] std::string_view application_name() const noexcept override { return "linux08-pad"; }

  // Can't print ava::core::Application.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

namespace terminal = ava::tui::terminal;

std::array<char const*, 10> lorem_ipsum_paragraphs = {
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Etiam lacinia, augue quis iaculis hendrerit, risus dui pellentesque augue, nec sodales ipsum "
    "felis at massa. Cras at leo elementum, egestas odio non, mattis dolor. Pellentesque elementum vestibulum diam, nec scelerisque eros hendrerit sed. Morbi "
    "quis lorem neque. Aliquam sollicitudin ante ipsum, id tempor lectus eleifend vitae. Fusce vel pulvinar metus. Sed sed blandit tellus. Etiam et lacinia "
    "lectus. Nunc posuere sit amet ante sit amet porttitor. Pellentesque quis auctor neque. Vivamus sollicitudin eleifend erat, ac sollicitudin augue bibendum "
    "sit amet. Praesent elementum fringilla elit eu hendrerit. Ut id consequat nisi. Vestibulum sapien dolor, ornare vitae dui ac, eleifend ultrices mauris.",
    "Mauris hendrerit dolor id velit gravida fringilla. Mauris porttitor venenatis odio, porttitor elementum nulla semper at. Morbi in massa fringilla, "
    "elementum mauris ut, aliquam urna. Aenean dapibus lobortis arcu ac hendrerit. Curabitur ac tellus nec ligula gravida gravida. In quis odio a erat cursus "
    "imperdiet. In rutrum lacinia nibh et bibendum. Vivamus eget lectus elementum, facilisis augue vel, luctus augue. Curabitur eget libero justo. Aliquam "
    "erat volutpat.",

    "Nullam rhoncus, sem nec congue vulputate, purus nulla tempus ipsum, at bibendum neque sapien cursus felis. Phasellus eu vulputate mi, congue pellentesque "
    "mi. In libero nunc, placerat quis maximus quis, volutpat ut magna. Nulla porttitor felis auctor mi posuere, sed congue sapien ultricies. In lobortis quam "
    "quis quam vestibulum, in egestas mi maximus. Mauris tristique felis lacinia risus tincidunt egestas. Ut nec erat lacinia, auctor leo mattis, malesuada "
    "nulla. Donec molestie id enim et facilisis. Sed tortor diam, ornare id egestas id, luctus in metus. Maecenas id tincidunt mi. Donec tempor viverra "
    "fringilla. Duis sollicitudin porta tellus vel cursus. Cras in imperdiet urna. Vestibulum interdum elementum hendrerit.",

    "Phasellus ac magna nec orci blandit tincidunt. Aenean finibus massa sed quam rutrum euismod. Morbi sollicitudin egestas malesuada. Vestibulum imperdiet, "
    "neque at iaculis congue, sem libero efficitur metus, in dapibus lectus ex consectetur nulla. Suspendisse volutpat, felis vel euismod rhoncus, ipsum massa "
    "sagittis quam, a efficitur mauris metus ut tellus. Donec lectus neque, commodo in elit et, consequat sodales sapien. Curabitur id odio ullamcorper, "
    "scelerisque lorem eget, porta leo. Quisque feugiat arcu quis sem vestibulum, eget bibendum sem dapibus. Sed eu ultricies urna. Morbi luctus mattis "
    "tellus, vitae vulputate ante scelerisque non. Aliquam erat volutpat. Nulla libero augue, ultrices sit amet justo volutpat, finibus maximus urna. Nam "
    "faucibus sed libero aliquam porta. Interdum et malesuada fames ac ante ipsum primis in faucibus. Cras pulvinar blandit urna, eget finibus lacus posuere "
    "non.",

    "Pellentesque fringilla velit vitae justo faucibus blandit. In pulvinar lectus ipsum, quis finibus odio convallis ut. Cras feugiat enim eget maximus "
    "auctor. Aliquam in tempus lectus, sed pharetra magna. Proin accumsan venenatis faucibus. Maecenas eget metus tempus, porta nulla at, rhoncus massa. Nam "
    "semper mollis mattis. Etiam lacinia odio odio, eu ullamcorper neque suscipit a. Donec vitae dui erat. Proin urna ligula, tincidunt nec orci eget, ornare "
    "blandit nisl. In mi velit, mattis non sapien vel, porttitor mollis lorem.",

    "Sed sem tellus, consequat eget dui ut, finibus tempor diam. Cras id eros lorem. In tincidunt, orci id pulvinar lobortis, augue dolor volutpat sem, id "
    "condimentum nisi risus sit amet dui. Pellentesque at fringilla nulla. Vivamus luctus consectetur commodo. Suspendisse potenti. Aliquam erat volutpat. "
    "Class aptent taciti sociosqu ad litora torquent per conubia nostra, per inceptos himenaeos. Quisque gravida auctor eros, at vulputate elit pretium a. "
    "Aenean tincidunt ex quis massa sodales, a commodo neque elementum. Aenean efficitur urna nibh, a egestas purus ullamcorper id. Suspendisse potenti. "
    "Morbi sodales semper leo non suscipit.",

    "Pellentesque habitant morbi tristique senectus et netus et malesuada fames ac turpis egestas. Suspendisse sit amet lacus nec odio iaculis tincidunt "
    "quis vel metus. Nullam sit amet diam imperdiet, egestas sapien eu, rutrum justo. In at orci tortor. Vestibulum vehicula vehicula est, et pharetra purus "
    "commodo pulvinar. Suspendisse potenti. Suspendisse dignissim rutrum sapien ut finibus. Donec bibendum ultricies dui, quis dignissim lacus mattis sit "
    "amet. Sed suscipit non neque vel auctor. Sed ac tellus quis eros imperdiet finibus ut ac nulla. Etiam ultrices, diam vel vehicula tincidunt, dolor metus "
    "feugiat orci, et ullamcorper mauris ante vel tortor.",

    "Mauris laoreet est arcu. Phasellus tortor nisi, commodo in faucibus ac, luctus cursus magna. Ut molestie leo vitae augue maximus, in interdum justo "
    "ornare. In hac habitasse platea dictumst. Aenean scelerisque iaculis suscipit. Cras interdum velit et justo commodo, eu elementum enim pellentesque. "
    "Fusce luctus egestas fermentum. Morbi consectetur justo id neque eleifend tristique id a lacus. Quisque sed sollicitudin odio, sed sollicitudin massa. "
    "Aliquam ac nulla vel turpis ultricies bibendum at sagittis orci. In iaculis vel ante et porttitor. Etiam eu turpis nec est iaculis elementum quis "
    "volutpat neque. Nulla volutpat, neque sit amet luctus faucibus, sapien nunc mollis neque, non eleifend neque libero sit amet dui. Donec a vehicula quam, "
    "mollis blandit turpis. Donec sed accumsan massa.",

    "Nulla rutrum vel nunc sit amet luctus. Nullam mattis ultrices diam vitae suscipit. Sed viverra pretium nibh, vel blandit lacus posuere nec. Orci varius "
    "natoque penatibus et magnis dis parturient montes, nascetur ridiculus mus. Nulla fringilla risus quis consectetur vehicula. Suspendisse eget nibh eu "
    "dolor mollis viverra. Mauris commodo ut odio in egestas. Vestibulum commodo felis vel libero viverra, sit amet interdum risus lacinia. Suspendisse "
    "ultrices ullamcorper metus, ullamcorper semper orci accumsan non. In ipsum velit, pharetra in odio vel, tristique vulputate dolor. Integer cursus gravida "
    "luctus. Nullam sapien arcu, luctus eget interdum in, tempor sit amet nisl. Vivamus ultrices vel urna et egestas. Aliquam nisl tellus, sodales et "
    "ultricies in, fringilla et mi.",

    "Sed justo magna, volutpat efficitur fermentum id, iaculis in sem. Cras laoreet massa tortor, eget sollicitudin mi auctor quis. Suspendisse libero urna, "
    "commodo vel molestie in, faucibus ullamcorper neque. Aenean dictum ultricies nulla convallis gravida. Praesent nec lorem ac nibh finibus dignissim at id "
    "dui. Mauris quis sodales turpis. Sed tristique eros ut justo blandit lobortis. Nulla eget lorem sapien. Praesent eros nibh, ullamcorper nec purus in, "
    "sodales finibus massa. Morbi elementum quis ligula sit amet dictum. Integer semper imperdiet sapien, non aliquet massa mattis sit amet. In aliquet, lacus "
    "vel aliquam gravida, libero lectus volutpat libero, ut scelerisque est libero nec odio. Sed vitae mattis dolor, eget placerat lorem. Fusce sapien turpis, "
    "vestibulum ut tortor vitae, euismod vestibulum metus. Donec non dignissim lectus."};

namespace {

constexpr uint32_t pad_line_width = 120;
constexpr int paragraph_repetitions = 20;
constexpr std::size_t highlight_every_n_words = 7;

// Convert ASCII lorem ipsum text into the UTF-8 string type that TextSpan::create expects.
std::u8string to_u8string(std::string_view text)
{
  return std::u8string{reinterpret_cast<char8_t const*>(text.data()), text.size()};
}

// Build one Paragraph from a single lorem ipsum paragraph by chopping it into phrases of
// alternating 51 and 19 characters long. Every third 19-character phrase gets its own colored
// Rendition.
std::unique_ptr<terminal::Paragraph> make_paragraph(std::string_view text, terminal::Rendition paragraph_rendition,
                                                    std::array<terminal::ColorPair, 4> const& phrase_colors)
{
  auto paragraph = terminal::Paragraph::create(paragraph_rendition, {.minimum_width = 16});

  int remaining = text.size();
  std::array<int, 2> pla{51, 19};
  bool need_color = false;
  int begin = 0;
  int phrase19_count = 0;
  int phrase19_index = 0;

  while (remaining > 0)
  {
    for (int pl = 0; pl < 2; ++pl)
    {
      int phrase_len = pla[pl];
      std::string_view phrase = text.substr(begin, phrase_len);
      begin += phrase_len;

      if (pl == 1 && need_color)
      {
        paragraph->append(terminal::TextSpan::create(to_u8string(phrase), terminal::Rendition{phrase_colors[phrase19_index % phrase_colors.size()]}));
        ++phrase19_index;
      }
      else
        paragraph->append(terminal::TextSpan::create(to_u8string(phrase)));

      remaining -= phrase_len;
      if (remaining <= 0)
        break;
    }
    ++phrase19_count;
    need_color = (phrase19_count + 1) % 3 == 0;
  }

  return paragraph;
}

} // namespace

int main()
{
  Debug(NAMESPACE_DEBUG::init());
  Debug(libcw_do.always_flush_on());

  std::mutex log_mutex;
  std::ofstream log("debug.out");
  Debug(libcw_do.set_ostream(&log, &log_mutex));

  [[maybe_unused]] Application application;
  terminal::Context terminal_context;
  terminal::BasicWindow const& stdscr = terminal_context.stdscr();

  std::array const paragraph_colors = {
      terminal_context.create_color_pair({0x301838}, {0xe8c8f0}),
      terminal_context.create_color_pair({0xe8e8e8}, {0x382030}),
      terminal_context.create_color_pair({0xa01408}, {0x50d8a8}),
      terminal_context.create_color_pair({0xd8f0e0}, {0x183828}),
  };
  std::array const phrase19_colors = {
      terminal_context.create_color_pair({0xfff060}, {0x804020}),
      terminal_context.create_color_pair({0x102850}, {0x70e0f0}),
      terminal_context.create_color_pair({0xffd8f0}, {0x903060}),
      terminal_context.create_color_pair({0x182008}, {0xa8e050}),
  };

  // Fill the Pad's with the lorem ipsum paragraphs, cycling through them (and through the
  // paragraph default renditions) to get enough content to scroll through.
  std::array<terminal::Pad, 3> pads;
  int const total_paragraph_count = paragraph_repetitions * static_cast<int>(lorem_ipsum_paragraphs.size());
  for (int paragraph_number = 0; paragraph_number < total_paragraph_count; ++paragraph_number)
  {
    int const source_index = paragraph_number % static_cast<int>(lorem_ipsum_paragraphs.size());
    for (int p = 0; p < pads.size(); ++p)
      pads[p].append(make_paragraph(lorem_ipsum_paragraphs[source_index], terminal::Rendition{paragraph_colors[paragraph_number % paragraph_colors.size()]},
                                    phrase19_colors));
  }

  // Wrap the content at pad_line_width cells and create the ncurses pad from it.
  for (int p = 0; p < pads.size(); ++p)
    pads[p].generate(pad_line_width);

  uint32_t const pad_view_height = 17;
  terminal::Position const top_left_first_pad_view{1, 5};
  std::array<terminal::Position, 3> pad_view_pos;
  for (int p = 0; p < pads.size(); ++p)
  {
    terminal::Margin const pad_view_offset{static_cast<uint8_t>(p * pad_view_height), 0};
    pad_view_pos[p] = top_left_first_pad_view + pad_view_offset;
  }
  terminal::Dimension const view_size{pad_view_height, std::min(pad_line_width, stdscr.getmaxyx().width() - top_left_first_pad_view.col())};

  int const max_first_row = static_cast<int>(pads[0].dimension().height() - pads.size() * view_size.height());
  int first_row = 0;
  for (;;)
  {
    for (int p = 0; p < pads.size(); ++p)
      pads[p].prefresh({static_cast<uint32_t>(first_row + p * pad_view_height), 0}, pad_view_pos[p], view_size);

    bool saw_esc = false;
    bool saw_bracket_open = false;
    int count = 0;
    for (;;)
    {
      wint_t key;
      // Input is screen-global, but reading it through stdscr can implicitly refresh
      // stdscr over the pad. Reading through the pad retains ncurses key decoding;
      // pads are deliberately not refreshed as a side effect of input operations.
      pads[0].basic_window().get_wch(key);

      if (key == 'q')
        return EXIT_SUCCESS;

      bool saw_scroll_up, saw_scroll_down;
      if (saw_esc && key == '[')
        saw_bracket_open = true;
      else
      {
        saw_scroll_up = false;
        saw_scroll_down = false;
        if (saw_bracket_open && (key == 'A' || key == 'B'))
        {
          saw_scroll_up = key == 'A';
          saw_scroll_down = key == 'B';
        }
        saw_bracket_open = false;
      }

      if (saw_scroll_up || saw_scroll_down)
      {
        if ((++count % 5) == 0)
        {
          first_row = std::clamp(first_row + (saw_scroll_down ? 1 : -1), 0, max_first_row);
          break;
        }
      }

      saw_esc = key == 27;
    }
  }
}

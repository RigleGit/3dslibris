#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "library/browser_draw_context.h"
#include "library/browser_grid_view.h"
#include "library/browser_list_view.h"
#include "library/browser_presentation_utils.h"
#include "shared/text_wrap_adapter.h"
#include "library/browser_view_utils.h"
#include "test_assert.h"

static const int TEXT_STYLE_REGULAR = 0;
static const int TEXT_STYLE_BOLD = 1;

struct Book {
  std::string title;
  bool IsBrowserFolder() const { return false; }
  int GetPosition() const { return 0; }
};

struct Text {
  struct Margins { int left = 0, right = 0; } margin;
  struct Display { int width = 240; } display;
  struct Printed { std::string text; int x, y; };
  std::vector<Printed> printed;
  int style = 0, x = 0, y = 0;
  bool clip = false, wrap = true;
  int GetColorMode() const { return 0; }
  int GetHeight() const { return 12; }
  int GetStyle() const { return style; }
  void SetStyle(int value) { style = value; }
  void SetPixelSize(int) {}
  void SetTextColorOverride(u16) {}
  void ClearTextColorOverride() {}
  void FillRect(u16, u16, u16, u16, u16) {}
  void DrawRect(u16, u16, u16, u16, u16) {}
  bool IsClipToContentEnabled() const { return clip; }
  bool IsAutoWrapEnabled() const { return wrap; }
  void SetClipToContentEnabled(bool value) { clip = value; }
  void SetAutoWrapEnabled(bool value) { wrap = value; }
  int GetCharCountInsideWidth(const char *text, int, int width) const {
    // Fixed host metrics isolate row layout from FreeType/GPU dependencies.
    return std::min((int)std::string(text).size(), width / 6);
  }
  void SetPen(int px, int py) { x = px; y = py; }
  void PrintString(const char *text, int = -1) {
    printed.push_back(Printed{std::string(text), x, y});
  }
};

BrowserGridMarqueeState::BrowserGridMarqueeState()
    : book(nullptr), strip(nullptr), bg_strip(nullptr), strip_w(0), strip_h(0),
      strip_x(0), strip_y(0), blit_w(0), scroll_offset(0), painted_offset(-1),
      scroll_timer(0), end_timer(0), color_mode(-1), bg_color(0xFFFF),
      active(false) {}
void BrowserGridMarqueeState::Reset() {
  book = nullptr;
  display_name.clear();
  color_mode = -1;
  active = false;
}
static int g_marquee_builds = 0;
static int g_marquee_x = 0, g_marquee_w = 0;
namespace browser_grid_view {
void BuildMarquee(Text *, BrowserGridMarqueeState &m, Book *book,
                  const std::string &name, unsigned char, int x, int, int w) {
  g_marquee_builds++;
  g_marquee_x = x;
  g_marquee_w = w;
  m.book = book;
  m.display_name = name;
  m.color_mode = 0;
  m.active = true;
}
}

namespace browser_presentation_utils {
std::string BuildBrowserDisplayName(Book *book) { return book->title; }
#include "browser_wrapped_title_under_test.inc"
}
namespace browser_list_view {
#include "browser_list_render_under_test.inc"
}

int main() {
  Book first{"Alpha Beta Gamma Delta Epsilon"};
  Book second{"Next book"};
  std::vector<Book *> books{&first, &second};
  for (Book *selected : {&first, &second}) {
    Text text;
    BrowserDrawContext ctx{&text, &books, selected, nullptr};
    BrowserGridMarqueeState marquee;
    browser_list_view::DrawPage(ctx, marquee, 0, 7);
    test::ExpectEq("two-line name does not scroll", g_marquee_builds, 0);
    std::vector<Text::Printed> title_lines;
    for (const auto &line : text.printed) {
      if (line.text != "NEW" && line.y <
          browser_list_view::kRowY0 + browser_list_view::kRowPitch)
        title_lines.push_back(line);
    }
    test::ExpectEq("list title uses both visible lines", title_lines.size(), 2);
    test::ExpectStrEq("first line preserves start", title_lines[0].text.c_str(),
                       "Alpha Beta Gamma Delta");
    test::ExpectStrEq("second line preserves end", title_lines[1].text.c_str(),
                       "Epsilon");
    test::ExpectTrue("first line stays within row",
                     title_lines[0].y - text.GetHeight() >=
                         browser_list_view::kRowY0);
    test::ExpectTrue("second line stays within row",
                     title_lines[1].y <= browser_list_view::kRowY0 +
                                                browser_list_view::kRowH);
  }

  // A name that needs more than two lines scrolls on the selected row only.
  Book long_book{"The Extraordinarily Long Title of a Remarkably Verbose Book "
                 "About Overflowing Labels"};
  Book other{"Short"};
  std::vector<Book *> long_books{&long_book, &other};
  {
    Text text;
    BrowserGridMarqueeState marquee;
    BrowserGridMarqueeState *m = &marquee;
    BrowserDrawContext ctx{&text, &long_books, &long_book, nullptr};
    browser_list_view::DrawPage(ctx, *m, 0, 7);
    test::ExpectEq("selected long name builds marquee", g_marquee_builds, 1);
    test::ExpectTrue("marquee stays inside the title area",
                     g_marquee_x >= browser_list_view::kRowX &&
                         g_marquee_x + g_marquee_w <=
                             browser_list_view::kRowX +
                                 browser_list_view::kRowW - 44);
    for (const auto &line : text.printed)
      test::ExpectTrue("scrolling row prints no static title",
                       line.text == "NEW" || line.y >=
                           browser_list_view::kRowY0 +
                               browser_list_view::kRowPitch);
    // Same state redraws without rebuilding the strip.
    browser_list_view::DrawPage(ctx, *m, 0, 7);
    test::ExpectEq("marquee is reused", g_marquee_builds, 1);
  }
  {
    // The same name on an unselected row wraps and builds no marquee.
    Text text;
    BrowserGridMarqueeState marquee;
    BrowserDrawContext ctx{&text, &long_books, &other, nullptr};
    browser_list_view::DrawPage(ctx, marquee, 0, 7);
    test::ExpectEq("unselected long name does not scroll", g_marquee_builds, 1);
    test::ExpectTrue("unselected long name still wraps", text.printed.size() >= 3);
  }
  return 0;
}

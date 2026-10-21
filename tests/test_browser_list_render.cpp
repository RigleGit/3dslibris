#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "library/browser_draw_context.h"
#include "library/browser_list_view.h"
#include "library/browser_presentation_utils.h"
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
    browser_list_view::DrawPage(ctx, 0, 7);
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
  return 0;
}

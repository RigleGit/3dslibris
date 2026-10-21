#include "reader/app_book_internal.h"
#include "app/app_mode.h"
#include "shared/orientation_utils.h"
#include "shared/string_utils.h"
#include "shared/text_token_constants.h"
#include "utf8proc.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

// Recording UI sink. Integer text measurement below is the actual Text method;
// glyph advances are fixed, and the legacy byte-width API saturates at 255.
class Text {
public:
  struct Printed {
    std::string value;
    int x, y;
  };
  std::vector<Printed> printed;
  u16 left{}, right{};
  u16 *screenleft = &left, *screenright = &right, *screen = screenleft;
  int width = 240, style = 2, color = 1, x = 0, y = 0;
  u8 orientation = 0, splash_orientation = 255;
  int GetStyle() {
    return style;
  }
  void SetStyle(int s) {
    style = s;
  }
  int GetColorMode() {
    return color;
  }
  void SetColorMode(int c) {
    color = c;
  }
  u16 *GetScreen() {
    return screen;
  }
  void SetScreen(u16 *s) {
    screen = s;
  }
  int LogicalWidth() {
    return width;
  }
  int LogicalWidthFor(bool) {
    return width;
  }
  void ClearScreen() {
    printed.clear();
  }
  void PrintSplash(u16 *) { splash_orientation = orientation; }
  void MarkAllScreensDirty() {}
  void SetPen(u16 a, u16 b) {
    x = a;
    y = b;
  }
  void PrintString(const char *s) {
    printed.push_back({s, x, y});
  }
  bool BlitToFramebuffer() {
    return true;
  }
  int GetAdvance(u32) {
    return 7;
  }
  u8 GetCharCode(const char *s, size_t length, u32 *out) {
    utf8proc_int32_t cp;
    const auto count =
        utf8proc_iterate((const utf8proc_uint8_t *)s, length, &cp);
    assert(count > 0);
    *out = cp;
    return count;
  }
  int GetStringAdvance(const char *s);
  u8 GetStringWidth(const char *s, u8) {
    return std::min(255, GetStringAdvance(s));
  }
};
#include "opening_text_measure.inc"
struct Book {
  std::string name =
      "Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota Kappa.epub";
  const char *GetFileName() {
    return name.c_str();
  }
  const char *GetTitle() {
    return "fallback";
  }
};
struct App {
  std::unique_ptr<Text> ts{new Text};
  Book book;
  u8 orientation = orientation_utils::ORIENT_LANDSCAPE;
  u8 portrait_orientation = orientation_utils::ORIENT_TURNED_LEFT;
  u8 render_orientation = orientation_utils::ORIENT_LANDSCAPE;
  bool landscape = true;
  struct { AppMode mode = AppMode::Opening; } nav_;
  struct { Book *bookcurrent = nullptr; } reader_state_;
  void ApplyRenderOrientation(u8 value) {
    render_orientation = value;
    ts->orientation = value;
    ts->width = value == orientation_utils::ORIENT_LANDSCAPE ? 320 : 240;
  }
  void SetOrientation(u8 value);
  void SetHandedness(u8 value);
  void ShowCurrentBookView();
  void RequestStatusRedraw() {}
  Book *GetSelectedBook() {
    return &book;
  }
  void DrawBottomGradientBackground() {}
  bool ShouldAbortWork() {
    return false;
  }
};
#include "opening_orientation_under_test.inc"
void gfxFlushBuffers() {}
void gfxSwapBuffers() {}
namespace reader_internal {
#include "opening_splash_under_test.inc"
}
void TestLoadingForHand(u8 handedness) {
  App app;
  auto &text = *app.ts;
  app.portrait_orientation = handedness;
  app.ApplyRenderOrientation(orientation_utils::ORIENT_LANDSCAPE);
  const int width = 240;
  reader_internal::DrawOpeningSplashImpl(&app, 1, 17, "opening book ...");
  assert(app.render_orientation == handedness);
  assert(text.splash_orientation == handedness);
  assert(app.orientation == orientation_utils::ORIENT_LANDSCAPE);
  assert(text.printed[1].value == "Alpha Beta Gamma Delta Epsilon");
  std::string reconstructed;
  for (const auto &p : text.printed)
    if (p.y >= 50 && p.y < 100) {
      assert(p.x == 12 && p.x + (int)p.value.size() * 7 <= width - 12);
      if (!reconstructed.empty())
        reconstructed += ' ';
      reconstructed += p.value;
    }
  assert(reconstructed == app.book.name);
  assert(text.printed.back().value == "1 / 17  (5%)");
  assert(text.style == 2 && text.color == 1 && text.screen == text.screenleft);
  // Cache-save progress can be drawn after a book was already in landscape.
  app.ApplyRenderOrientation(orientation_utils::ORIENT_LANDSCAPE);
  reader_internal::DrawOpeningSplashImpl(&app, 0, 0, "saving cache...");
  assert(text.splash_orientation == handedness);
  assert(app.render_orientation == handedness);
  assert(text.printed.front().value == "saving cache...");
  // The last token still needs clipping, even when no more words follow it.
  app.book.name = "Intro " + std::string(150, 'W');
  reader_internal::DrawOpeningSplashImpl(&app, 0, 0, nullptr);
  for (const auto &p : text.printed)
    if (p.y >= 50 && p.y < 100)
      assert(p.x + (int)p.value.size() * 7 <= width - 12);
  // Never cut a multi-byte filename in the middle of a character.
  app.book.name.clear();
  for (int i = 0; i < 150; ++i)
    app.book.name += "年";
  reader_internal::DrawOpeningSplashImpl(&app, 0, 0, nullptr);
  for (const auto &p : text.printed)
    if (p.y >= 50 && p.y < 100) {
      size_t pos = 0;
      int characters = 0;
      while (pos < p.value.size()) {
        utf8proc_int32_t cp;
        const auto bytes =
            utf8proc_iterate((const utf8proc_uint8_t *)p.value.data() + pos,
                             p.value.size() - pos, &cp);
        assert(bytes > 0);
        pos += bytes;
        ++characters;
      }
      assert(characters * 7 <= width - 24);
    }
}
void TestOpeningToReaderTransition() {
  for (u8 hand : {u8(0), u8(1)}) {
    App app;
    app.portrait_orientation = hand;
    app.SetOrientation(orientation_utils::ORIENT_LANDSCAPE);
    assert(app.render_orientation == hand);
    assert(app.orientation == orientation_utils::ORIENT_LANDSCAPE);
    app.SetHandedness(u8(1 - hand));
    assert(app.render_orientation == 1 - hand);
    assert(app.orientation == orientation_utils::ORIENT_LANDSCAPE);
    app.reader_state_.bookcurrent = &app.book;
    app.ShowCurrentBookView();
    assert(app.nav_.mode == AppMode::Book);
    assert(app.render_orientation == orientation_utils::ORIENT_LANDSCAPE);
    app.SetOrientation(orientation_utils::ORIENT_LANDSCAPE);
    assert(app.render_orientation == orientation_utils::ORIENT_LANDSCAPE);
  }
}
int main() {
  TestLoadingForHand(0);
  TestLoadingForHand(1);
  TestOpeningToReaderTransition();
  puts("PASS: opening stays portrait until reading, with bounded title measurements "
       "and valid UTF-8");
}

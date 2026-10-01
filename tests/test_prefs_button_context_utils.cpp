#include "settings/prefs_button_context_utils.h"
#include "test_assert.h"
#include "settings/prefs_action_utils.h"
#include "settings/prefs_input_utils.h"
#include <cassert>
#define DBG_LOGF(...) ((void)0)
struct Prefs {
  bool time24h=true; int writes=0;
  void RequestWrite() { ++writes; }
};
struct Text {
  int color=0, redraws=0;
  int GetColorMode() const { return color; }
  void SetColorMode(int v) { color=v; }
  void MarkAllScreensDirty() { ++redraws; }
};
struct App {
  Prefs *prefs=nullptr; bool reopen=false; int colorMode=0, layout_dirty=0;
  bool publisher_text_indent=false, publisher_block_margins=false, publisher_horizontal_margins=false;
  void MarkBookLayoutDirty() { ++layout_dirty; }
};
struct Book {
  bool reflowable=true; int indent=-1, block=-1, horizontal=-1;
  bool UsesTextLayoutSettings() const { return reflowable; }
  int GetStylePublisherTextIndentOverride() const { return indent; }
  int GetStylePublisherBlockMarginsOverride() const { return block; }
  int GetStylePublisherHorizontalMarginsOverride() const { return horizontal; }
  void SetStylePublisherTextIndentOverride(int v) { indent=v; }
  void SetStylePublisherBlockMarginsOverride(int v) { block=v; }
  void SetStylePublisherHorizontalMarginsOverride(int v) { horizontal=v; }
};
static int skin_color=-1;
void UiButtonSkin_SetColorMode(int v) { skin_color=v; }
#include "prefs_actions_under_test.inc"

namespace {

bool HasButton(const settings::PrefsPageContext &page, int button) {
  const unsigned char count = settings::PrefsPageButtonCount(page);
  for (unsigned char slot = 0; slot < count; ++slot) {
    if (settings::PrefsPageButtonForSlot(page, slot) == button)
      return true;
  }
  return false;
}

void CheckDrawablePage(const settings::PrefsPageContext &page) {
  const unsigned char count = settings::PrefsPageButtonCount(page);
  test::ExpectTrue("page has between one and eight rows", count > 0 && count <= 8);
  const int pitch = settings::PrefsRowPitch(count);
  // The footer is at y=296 with eight pixels of upward touch slack.
  test::ExpectTrue("last row clears footer touch region",
                   (count - 1) * pitch + (pitch - 2) < 288);
  for (unsigned char slot = 0; slot < count; ++slot) {
    const int id = settings::PrefsPageButtonForSlot(page, slot);
    test::ExpectTrue("row indexes an existing preference button",
                     id >= 0 && id < PREFS_BUTTON_COUNT);
    for (unsigned char previous = 0; previous < slot; ++previous) {
      test::ExpectNe("a preference cannot occupy two rows", id,
                     settings::PrefsPageButtonForSlot(page, previous));
    }
  }
}

} // namespace

static void TestSettingsActions() {
  App app; Prefs prefs; Text text; Book book; app.prefs=&prefs;
  ToggleClockFormatSetting(&prefs);
  assert(!prefs.time24h && prefs.writes == 1);
  ToggleClockFormatSetting(&prefs);
  assert(prefs.time24h && prefs.writes == 2);
  ToggleReopenLastBookSetting(&app);
  assert(app.reopen && prefs.writes == 3);
  // The real action keeps text, button skin and App color synchronized.
  for (int color : {2, 5, -1, 8}) {
    text.color=color; const int redraws=text.redraws;
    CycleColorMode(&text, &app);
    const int expected=color == 2 ? 3 : 0;
    assert(text.color == expected && app.colorMode == expected && skin_color == expected);
    assert(text.redraws == redraws + 1);
  }
  // Per-book tri-state changes must leave global defaults untouched and
  // request both relayout and a pending write through the actual actions.
  void (*actions[])(App *, Book *, bool) = {
      TogglePublisherTextIndentSetting, TogglePublisherBlockMarginsSetting,
      TogglePublisherHorizontalMarginsSetting};
  for (auto action : actions) {
    app.publisher_text_indent=app.publisher_block_margins=app.publisher_horizontal_margins=false;
    for (int value : {-1, 0, 1, 4}) {
      book.indent=book.block=book.horizontal=value;
      const int writes=prefs.writes, dirty=app.layout_dirty;
      action(&app, &book, true);
      const int changed=action == TogglePublisherTextIndentSetting ? book.indent
          : action == TogglePublisherBlockMarginsSetting ? book.block : book.horizontal;
      assert(changed == (value < 0 ? 0 : value == 0 ? 1 : -1));
      assert(!app.publisher_text_indent && !app.publisher_block_margins && !app.publisher_horizontal_margins);
      assert(prefs.writes == writes + 1 && app.layout_dirty == dirty + 1);
    }
    action(&app, nullptr, false);
    const bool enabled=action == TogglePublisherTextIndentSetting ? app.publisher_text_indent
        : action == TogglePublisherBlockMarginsSetting ? app.publisher_block_margins : app.publisher_horizontal_margins;
    assert(enabled);
  }
  const int writes=prefs.writes;
  ToggleClockFormatSetting(nullptr); ToggleReopenLastBookSetting(nullptr);
  TogglePublisherTextIndentSetting(nullptr, &book, true);
  CycleColorMode(nullptr, &app);
  assert(prefs.writes == writes);
}
static void TestSettingsInputContext() {
  const uint32_t back=1, select=2, y=4, start=8, action=16;
  for (bool book : {false, true}) {
    for (uint32_t key : {back, select, y, start, action}) {
      assert(prefs_input_utils::ShouldReturnFromPrefs(key, book, back, select, y, start)
          == (key != action && (key != start || !book)));
    }
  }
  // This retains the independent overlay mode-admission contract; it does
  // not simulate the go-to-page dialog or runtime settings controller.
  for (bool dirty : {false, true}) {
    assert(prefs_input_utils::ShouldRedrawPrefsAfterOverlayInput(dirty, true) == dirty);
    assert(!prefs_input_utils::ShouldRedrawPrefsAfterOverlayInput(dirty, false));
  }
}
int main() {
  TestSettingsActions();
  TestSettingsInputContext();
  settings::PrefsPageContext page;
  test::ExpectEq("new settings view starts on the general page",
                 settings::PrefsPageButtonForSlot(page, 0),
                 PREFS_BUTTON_STYLE_CUSTOMIZATION);
  // A book may still be open while viewing general preferences. Its format
  // must not hide global options or expose per-book navigation actions.
  for (int flags = 0; flags < 4; ++flags) {
    page.fixed_layout = (flags & 1) != 0;
    page.include_line_wrap_fix = (flags & 2) != 0;
    for (int p = 0; p < 3; ++p) {
      page.page = p;
      CheckDrawablePage(page);
      test::ExpectFalse("general preferences exclude bookmarks",
                        HasButton(page, PREFS_BUTTON_BOOKMARKS));
      test::ExpectFalse("general preferences exclude book information",
                        HasButton(page, PREFS_BUTTON_BOOK_INFO));
    }
    page.page = 1;
    test::ExpectTrue("global style controls keep font configuration",
                     HasButton(page, PREFS_BUTTON_FONT_CONFIG));
    test::ExpectTrue("global style controls keep side margins",
                     HasButton(page, PREFS_BUTTON_PUBLISHER_HORIZONTAL_MARGINS));
    page.page = 2;
    test::ExpectTrue("global options keep reopen-last-book",
                     HasButton(page, PREFS_BUTTON_REOPEN_LAST_BOOK));
    test::ExpectTrue("Circle Pad remains a global option",
                     HasButton(page, PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN));
    test::ExpectTrue("global options keep reset and cache cleanup",
                     HasButton(page, PREFS_BUTTON_RESET_DEFAULTS) &&
                     HasButton(page, PREFS_BUTTON_CLEAR_CACHE));
  }

  page.from_book = true;
  for (int fixed = 0; fixed < 2; ++fixed) {
    page.fixed_layout = fixed != 0;
    for (int wrap = 0; wrap < 2; ++wrap) {
      page.include_line_wrap_fix = wrap != 0;
      page.page = 0;
      CheckDrawablePage(page);
      test::ExpectTrue("book navigation remains available",
                       HasButton(page, PREFS_BUTTON_BOOK_INFO) &&
                       HasButton(page, PREFS_BUTTON_INDEX) &&
                       HasButton(page, PREFS_BUTTON_BOOKMARKS));
      test::ExpectFalse("book page excludes global Circle Pad setting",
                        HasButton(page, PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN));
      test::ExpectTrue("optional line-wrap slot is present only when requested",
                       HasButton(page, PREFS_BUTTON_LIBRARY_VIEW) == (wrap != 0));
      if (wrap) {
        test::ExpectEq("line-wrap action precedes normal book controls",
                       settings::PrefsPageButtonForSlot(page, 1),
                       PREFS_BUTTON_LIBRARY_VIEW);
      }
      page.page = 1;
      CheckDrawablePage(page);
      test::ExpectTrue("both layouts retain orientation and handedness",
                       HasButton(page, PREFS_BUTTON_ORIENTATION) &&
                       HasButton(page, PREFS_BUTTON_HANDEDNESS));
      test::ExpectFalse("style page excludes global Circle Pad setting",
                        HasButton(page, PREFS_BUTTON_CIRCLE_PAD_PAGE_TURN));
      test::ExpectTrue("reading direction belongs to fixed-layout settings",
                       HasButton(page, PREFS_BUTTON_LIBRARY_VIEW) == (fixed != 0));
      const int typography[] = {PREFS_BUTTON_FONTSIZE, PREFS_BUTTON_LINE_SPACING,
          PREFS_BUTTON_PARASPACING, PREFS_BUTTON_PUBLISHER_TEXT_INDENT,
          PREFS_BUTTON_PUBLISHER_BLOCK_MARGINS,
          PREFS_BUTTON_PUBLISHER_HORIZONTAL_MARGINS};
      for (int id : typography) {
        test::ExpectTrue("typography settings belong to reflow books",
                         HasButton(page, id) == (fixed == 0));
      }
    }
  }
  return 0;
}

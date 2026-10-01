#include "reader/reader_controls.h"
#include "app/frame_input.h"
#include "formats/common/pdf_view_utils.h"
#include "shared/orientation_utils.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <vector>
struct circlePosition { int dx, dy; };
struct touchPosition { unsigned short px, py; };
static const uint32_t KEY_TOUCH=1u << 20;
static circlePosition pad={0, 0}, stick={0, 0};
void hidCircleRead(circlePosition *p) { *p=pad; }
void hidCstickRead(circlePosition *p) { *p=stick; }
static uint64_t now=100;
uint64_t osGetTime() { return now; }
struct Prefs { void Write() {} };
struct App {
  bool touch=false, new3ds=true;
  unsigned char orientation=orientation_utils::ORIENT_TURNED_LEFT;
  int last_x=-1, last_y=-1;
  uint64_t ready_at=0;
  struct { uint32_t r=0, l=0, zl=0, zr=0; } key;
  Prefs settings; Prefs *prefs=&settings;
  bool IsPdfTouchDragActive() const { return touch; }
  bool IsNew3dsDevice() const { return new3ds; }
  void SetPdfTouchDragActive(bool v) { touch=v; }
  void SetPdfTouchLastX(int v) { last_x=v; }
  void SetPdfTouchLastY(int v) { last_y=v; }
  int GetPdfTouchLastX() const { return last_x; }
  int GetPdfTouchLastY() const { return last_y; }
  void SetPdfDeferredReadyAtMs(uint64_t v) { ready_at=v; }
  uint64_t GetPdfDeferredReadyAtMs() const { return ready_at; }
  touchPosition MapTouch(const FrameInput &i) { return {(unsigned short)i.touch_raw_x, (unsigned short)i.touch_raw_y}; }
  void ShowLibraryView() {}
  void ShowSettingsView(bool) {}
};
struct Book {
  bool interaction=false, can_move=true, pending=false;
  format_t format=FORMAT_PDF;
  std::vector<int> chapters;
  const std::vector<int> &GetChapters() const { return chapters; }
};
struct Text {};
static std::vector<bool> draw_interactions;
static int pumps=0, pans=0, touch_moves=0;
static uint32_t deferred_delay=20;
static bool turn_succeeds=false, zoom_succeeds=false;
static int turn_calls=0, turn_direction=0, zoom_step=0;
namespace book_renderer {
bool TranslateFixedLayoutViewport(Book *b, float, float) { ++pans; return b->can_move; }
void SetFixedLayoutViewportInteraction(Book *b, bool active) { b->interaction=active; }
uint32_t GetFixedLayoutDeferredDelayMs(Book *) { return deferred_delay; }
bool ChangeFixedLayoutZoom(Book *, int step) { zoom_step=step; return zoom_succeeds; }
bool JumpFixedLayoutChapter(Book *, int) { return false; }
void ResetFixedLayoutViewportForNavigation(Book *) {}
bool MoveFixedLayoutViewportToPreview(Book *b, int, int) { ++touch_moves; return b->can_move; }
bool HasPendingFixedLayoutDeferredWork(Book *b) { return b->pending; }
bool PumpDeferredFixedLayoutWork(Book *, uint32_t budget) { assert(budget == 4); ++pumps; return true; }
}
namespace book_nav {
void DrawPage(Book *b, Text *) { draw_interactions.push_back(b->interaction); }
bool TurnPage(Book *, Text *, uint16_t *, uint16_t, int direction) {
  ++turn_calls; turn_direction=direction; return turn_succeeds;
}
}
#include "pad_helpers.inc"
namespace fixed_layout_input {
#include "pad_handler.inc"
}
static bool Frame(App &app, Book &book, Text &text, uint32_t down=0, uint32_t held=0, int x=0, int y=0, const ReaderControls *controls=nullptr) {
  const FrameInput input(down, held, (held & KEY_TOUCH) != 0, x, y, now);
  const ReaderControls empty={};
  const ReaderControls &ctrl=controls ? *controls : empty; uint16_t page=0;
  return fixed_layout_input::HandleInBook(app, &book, &text, input, &page, 1, ctrl);
}
struct TestKeys {
  uint32_t a, b, l, r, up, down, left, right;
  uint32_t dup, ddown, dleft, dright, zl, zr, start, select;
};
static void TestControlRouting() {
  const TestKeys key={1u<<0, 1u<<1, 1u<<2, 1u<<3, 1u<<4, 1u<<5, 1u<<6, 1u<<7,
      1u<<8, 1u<<9, 1u<<10, 1u<<11, 1u<<12, 1u<<13, 1u<<14, 1u<<15};
  const ReaderControls portrait=BuildPortraitControls(key);
  const ReaderControls landscape=BuildLandscapeControls(key);
  // Distinct reflow constructor/repeat contracts remain focused mask checks.
  // They do not claim to execute the reflowable reader handler.
  assert(portrait.page_next == (key.a | key.r | key.down | key.ddown | key.zl));
  assert(portrait.page_prev == (key.b | key.l | key.up | key.dup | key.zr));
  assert(portrait.bookmark_next == (key.left | key.dleft));
  assert(portrait.bookmark_prev == (key.right | key.dright));
  assert(landscape.page_next == (key.a | key.r | key.right | key.dright | key.zl));
  assert(landscape.page_prev == (key.b | key.l | key.left | key.dleft | key.zr));
  assert(landscape.bookmark_next == (key.down | key.ddown) && landscape.link_next == landscape.bookmark_next);
  assert(landscape.bookmark_prev == (key.up | key.dup) && landscape.link_prev == landscape.bookmark_prev);
  assert(reader_input_utils::ReflowablePageRepeatKeys(key.down, key.ddown, true) == (key.down | key.ddown));
  assert(reader_input_utils::ReflowablePageRepeatKeys(key.down, key.ddown, false) == key.ddown);
  App app; Book book; Text text; app.key.r=key.r; app.key.l=key.l; app.key.zl=key.zl; app.key.zr=key.zr;
  turn_succeeds=true;
  for (const ReaderControls *controls : {&portrait, &landscape}) {
    for (format_t format : {FORMAT_PDF, FORMAT_CBZ, FORMAT_UNDEF, FORMAT_EPUB}) {
      book.format=format;
      for (uint32_t shoulder : {key.r, key.zl, key.l, key.zr}) {
        const int before=turn_calls;
        const bool eligible=format == FORMAT_PDF || format == FORMAT_CBZ;
        assert(Frame(app, book, text, shoulder, 0, 0, 0, controls) == eligible);
        assert(turn_calls == before + (eligible ? 1 : 0));
        if (eligible) assert(turn_direction == ((shoulder == key.r || shoulder == key.zl) ? 1 : -1));
      }
    }
    book.format=FORMAT_PDF;
    for (uint32_t key_down : {key.dright, key.dleft, key.ddown, key.dup}) {
      const int before=turn_calls;
      assert(Frame(app, book, text, key_down, 0, 0, 0, controls));
      assert(turn_calls == before + 1);
      assert(turn_direction == ((key_down == key.dright || key_down == key.ddown) ? 1 : -1));
    }
    zoom_succeeds=true;
    assert(Frame(app, book, text, key.a, 0, 0, 0, controls) && zoom_step == 1);
    assert(Frame(app, book, text, key.b, 0, 0, 0, controls) && zoom_step == -1);
    zoom_succeeds=false;
  }
  turn_succeeds=false; draw_interactions.clear();
}
int main() {
  TestControlRouting();
  App app; Book book; Text text;
  assert(!Frame(app, book, text));
  pad.dx=60;
  assert(Frame(app, book, text)); assert(Frame(app, book, text));
  assert((draw_interactions == std::vector<bool>{true, true}));
  assert(app.ready_at == now + 20 && pumps == 0);
  pad.dx=0;
  assert(Frame(app, book, text));
  assert((draw_interactions == std::vector<bool>{true, true, false}));
  assert(!Frame(app, book, text) && draw_interactions.size() == 3);
  stick.dy=30; assert(Frame(app, book, text));
  book.can_move=false; assert(!Frame(app, book, text));
  stick.dy=0; assert(Frame(app, book, text));
  assert(!draw_interactions.back());
  pad.dx=15; stick.dy=8; // Both exact dead-zone boundaries.
  assert(!Frame(app, book, text));
  pad.dx=0; stick.dy=30; app.new3ds=false;
  assert(!Frame(app, book, text)); // Old hardware must not read C-Stick.
  app.new3ds=true; stick.dy=0; book.can_move=true; pad.dx=60;
  assert(Frame(app, book, text));
  const int pad_calls=pans;
  assert(Frame(app, book, text, KEY_TOUCH, KEY_TOUCH, 30, 40));
  assert(app.touch && app.last_x == 30 && app.last_y == 40 && pans == pad_calls);
  const size_t before_hold=draw_interactions.size();
  assert(!Frame(app, book, text, 0, KEY_TOUCH, 32, 42));
  assert(draw_interactions.size() == before_hold && touch_moves == 1);
  assert(Frame(app, book, text, 0, KEY_TOUCH, 35, 40));
  pad.dx=0;
  assert(Frame(app, book, text)); // Actual touch-release branch owns one draw.
  assert(!app.touch && app.last_x == -1 && app.last_y == -1 && !draw_interactions.back());
  const size_t after_release=draw_interactions.size();
  assert(!Frame(app, book, text) && draw_interactions.size() == after_release);
  book.pending=true; now=app.ready_at-1;
  assert(!Frame(app, book, text) && pumps == 0);
  now=app.ready_at;
  assert(!Frame(app, book, text, 1u) && pumps == 0); // Input takes priority over idle work.
  assert(Frame(app, book, text) && pumps == 1 && !draw_interactions.back());
  assert(app.ready_at == now + 20);
  now=app.ready_at; pad.dx=60;
  assert(Frame(app, book, text) && pumps == 1); // A pan redraw defers expensive work.
  puts("PASS: full input handler schedules interaction/release draws and eligible idle work");
}

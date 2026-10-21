#include "reader/image_viewer_session.h"
#include "reader/image_view_utils.h"
#include "app/frame_input.h"
#include "app/app_mode.h"
#include "formats/common/fixed_layout_screen_constants.h"
#include "shared/orientation_utils.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

enum {
  KEY_A = 1,
  KEY_B = 2,
  KEY_X = 4,
  KEY_Y = 8,
  KEY_START = 16,
  KEY_SELECT = 32,
  KEY_DLEFT = 64,
  KEY_DRIGHT = 128,
  KEY_DUP = 256,
  KEY_DDOWN = 512,
  KEY_TOUCH = 1024
};
struct circlePosition {
  int dx = 0, dy = 0;
};
void hidCircleRead(circlePosition *p) {
  *p = circlePosition();
}
#define DBG_LOGF(...) ((void)0)
struct Skin {
  int x, y, w, h;
  UiButtonSkinState state;
};
static std::vector<Skin> skins;
bool UiButtonSkin_Init() {
  return true;
}
UiButtonIconId UiButtonSkin_IconFromLabel(const char *) {
  return UI_BUTTON_ICON_NONE;
}
int UiButtonSkin_IconBlockWidth(int) {
  return 0;
}
void UiButtonSkin_Draw(u16 *, int, int, int, int x, int y, int w, int h,
                       UiButtonSkinState state, bool) {
  skins.push_back({x, y, w, h, state});
}
void UiButtonSkin_DrawIcon(u16 *, int, int, int, int, int, int, int,
                           UiButtonIconId, bool) {}
namespace raster_image {
void FreePixels::operator()(uint16_t *p) const {
  delete[] p;
}
} // namespace raster_image
static std::vector<bool> qualities;
namespace fixed_layout_blit_utils {
void BlitRgb565BitmapScaledCrop(Text *, u16 *, int, int, int, int, int,
                                const u16 *, size_t, int, int, int, int, int,
                                int, bool quality) {
  qualities.push_back(quality);
}
} // namespace fixed_layout_blit_utils
struct Page {
  enum { IMAGE };
  struct Item {
    int kind;
    unsigned token_offset;
    u16 image_id;
  };
  std::vector<Item> items{{IMAGE, 0, 1}};
  const std::vector<Item> &GetRenderedSelectables() const {
    return items;
  }
};
struct Book {
  Page page;
  const char *GetFileName() {
    return "book.epub";
  }
  bool IsMobiFile() {
    return false;
  }
  unsigned GetPageCount() {
    return 1;
  }
  int GetPosition() {
    return 0;
  }
  bool IsAsyncReflowOpenPending() {
    return false;
  }
  Page *GetPage() {
    return &page;
  }
  int GetFocusedInlineLinkIndex() {
    return 0;
  }
  unsigned GetOpenSessionId() {
    return 1;
  }
  unsigned GetLayoutRevision() {
    return 1;
  }
};
bool HasExtCI(const char *, const char *) {
  return true;
}
struct App {
  Book book;
  std::unique_ptr<Text> ts{new Text};
  struct Keys {
    u32 a = KEY_A, b = KEY_B, x = KEY_X, y = KEY_Y, start = KEY_START,
        select = KEY_SELECT;
  } key;
  unsigned char orientation = 0;
  AppMode mode = AppMode::Book;
  bool abort = false;
  Book *GetCurrentBook() {
    return &book;
  }
  bool ShouldAbortWork() {
    return abort;
  }
  bool BookNeedsRelayout(Book *) {
    return false;
  }
  void ApplyRenderOrientation(unsigned char value) {
    ts->landscape = value == 2;
  }
  void ResetPageRepeat() {}
  void SetMode(AppMode value) {
    mode = value;
  }
  void PrintStatus(const char *) {}
  void ShowLibraryView() {
    mode = AppMode::Browser;
  }
  void ShowSettingsView(bool) {
    mode = AppMode::Prefs;
  }
};
struct ReaderController {
  App &app_;
  std::unique_ptr<ReaderImageViewerSession> image_viewer_;
  explicit ReaderController(App &app) : app_(app) {}
  bool ImageViewerValid() const {
    return image_viewer_ != nullptr;
  }
  void InvalidateSearch(bool) {}
  void InvalidateImageViewer() {
    image_viewer_.reset();
  }
  void CloseImageViewer() {
    image_viewer_.reset();
    app_.mode = AppMode::Book;
  }
  bool LoadImageViewer(int zoom) {
    image_viewer_->viewport.zoom_index = zoom;
    return true;
  }
  void OpenImageViewer();
  void DrawImageViewer();
  void RunImageViewerFrame(const FrameInput &);
  void SuspendImageViewer();
};
#include "image_viewer_under_test.inc"

struct Fixture {
  App app;
  ReaderController reader{app};
  explicit Fixture(unsigned char orientation) {
    app.orientation = orientation;
    reader.OpenImageViewer();
    auto &s = *reader.image_viewer_;
    s.loading = s.wait_release = false;
    s.image.width = s.image.height = s.image.original_width =
        s.image.original_height = 1000;
    s.image.pixels.reset(new u16[1000 * 1000]());
    Draw();
  }
  void Draw() {
    skins.clear();
    app.ts->printed.clear();
    qualities.clear();
    reader.DrawImageViewer();
  }
  void Input(u32 down, u32 held = 0, int x = 0, int y = 0) {
    // Independent inverse mapping from logical touch coordinates to raw panel.
    int rx = x, ry = y;
    if (app.orientation == 0) {
      rx = 319 - y;
      ry = x;
    } else if (app.orientation == 1) {
      rx = y;
      ry = 239 - x;
    }
    reader.RunImageViewerFrame(
        FrameInput(down, held, (held & KEY_TOUCH) != 0, rx, ry, 0));
  }
  Skin Control(const char *label) {
    for (const auto &p : app.ts->printed)
      if (p.text == label)
        for (const auto &b : skins)
          if (p.x > b.x && p.x < b.x + b.w && p.y > b.y && p.y <= b.y + b.h)
            return b;
    fprintf(stderr, "Missing clickable control: %s\n", label);
    assert(false);
    return {};
  }
  void Tap(const Skin &b) {
    Input(KEY_TOUCH, KEY_TOUCH, b.x + b.w / 2, b.y + b.h / 2);
  }
};
void TestLayout() {
  for (unsigned char orientation : {0, 1, 2}) {
    Fixture f(orientation);
    for (int mode : {0, 1, 2}) {
      auto &s = *f.reader.image_viewer_;
      if (mode == 1)
        s.message = std::string(300, 'W');
      if (mode == 2) {
        s.image = raster_image::Bitmap();
        s.message = "Image unavailable (resource/memory limit)";
      }
      f.Draw();
      const int width = orientation == 2 ? 320 : 240,
                height = orientation == 2 ? 240 : 320;
      assert(skins.size() == 6);
      for (const char *label :
           {"zoom +", "zoom -", "fit", "back", "library", "settings"})
        f.Control(label);
      for (const char *key :
           {"(A)", "(B)", "(X)", "(Y)", "(START)", "(SELECT)"}) {
        bool found = false;
        for (const auto &p : f.app.ts->printed)
          found |= p.text == key;
        assert(found);
      }
      for (const auto &b : skins) {
        assert(b.x >= 0 && b.y >= 0 && b.x + b.w <= width &&
               b.y + b.h <= height);
        if (mode != 2)
          assert(s.overview.y + s.overview.height < b.y);
      }
      for (const auto &p : f.app.ts->printed) {
        assert(p.screen == f.app.ts->screenright);
        for (const auto &b : skins) {
          if (p.x > b.x && p.x < b.x + b.w && p.y > b.y && p.y <= b.y + b.h) {
            // Leave 6px below glyphs, including the 2px descenders/parens,
            // so the shortcut clears the rounded button border.
            assert(p.y + 2 <= b.y + b.h - 6);
            assert(p.x + (int)p.text.size() * 7 <= b.x + b.w);
          }
        }
        assert(p.x >= 0 && p.x + (int)p.text.size() * 7 <= width &&
               p.y <= height);
      }
      assert(f.app.ts->GetPixelSize() == 14 && f.app.ts->margin.bottom == 36 &&
             f.app.ts->wrap);
    }
  }
}
void TestActions() {
  for (unsigned char orientation : {0, 1, 2}) {
    for (bool touch : {false, true}) {
      Fixture f(orientation);
      auto &s = *f.reader.image_viewer_;
      auto action = [&](const char *label, u32 key) {
        f.Draw();
        if (touch)
          f.Tap(f.Control(label));
        else
          f.Input(key, key);
        f.Input(0);
      };
      action("zoom +", KEY_A);
      assert(s.viewport.zoom_index == 3);
      assert(s.viewport.center_x == 0.5f && s.viewport.center_y == 0.5f);
      action("zoom -", KEY_B);
      assert(s.viewport.zoom_index == 2);
      s.viewport.zoom_index = 4;
      s.viewport.center_x = 0.8f;
      action("fit", KEY_X);
      assert(s.viewport.zoom_index == 2 && s.viewport.center_x == 0.5f);
      for (int i = 0; i < 8; ++i)
        action("zoom +", KEY_A);
      assert(s.viewport.zoom_index == 6);
      for (int i = 0; i < 8; ++i)
        action("zoom -", KEY_B);
      assert(s.viewport.zoom_index == 2);
      action("back", KEY_Y);
      assert(f.app.mode == AppMode::Book && !f.reader.image_viewer_);
      for (const auto &target : std::vector<std::pair<const char *, u32>>{
               {"library", KEY_START}, {"settings", KEY_SELECT}}) {
        Fixture other(orientation);
        if (touch)
          other.Tap(other.Control(target.first));
        else
          other.Input(target.second, target.second);
        assert(other.app.mode == (target.second == KEY_START ? AppMode::Browser
                                                             : AppMode::Prefs));
        assert(!other.reader.image_viewer_);
      }
    }
  }
}
void TestUnavailableControls() {
  for (unsigned char orientation : {0, 1, 2}) {
    for (const char *destination : {"back", "library", "settings"}) {
      Fixture f(orientation);
      auto &s = *f.reader.image_viewer_;
      s.image = raster_image::Bitmap();
      s.message = "Image unavailable";
      f.Draw();
      for (const char *label : {"zoom +", "zoom -", "fit"}) {
        const Skin b = f.Control(label);
        assert(b.state == UI_BUTTON_STATE_DISABLED);
        f.Tap(b);
        f.Input(0);
        assert(s.viewport.zoom_index == 2 && !s.viewport.interaction_active);
      }
      f.Tap(f.Control(destination));
      assert(!f.reader.image_viewer_);
      assert(f.app.mode == (strcmp(destination, "back") == 0 ? AppMode::Book
                            : strcmp(destination, "library") == 0
                                ? AppMode::Browser
                                : AppMode::Prefs));
    }
  }
}
void TestTouchOwnership() {
  for (unsigned char orientation : {0, 1, 2}) {
    Fixture f(orientation);
    auto &s = *f.reader.image_viewer_;
    s.viewport.zoom_index = 4;
    f.Draw();
    f.Input(KEY_TOUCH, KEY_TOUCH, 0, 0);
    assert(s.viewport.center_x == 0.5f && s.viewport.center_y == 0.5f &&
           !s.viewport.interaction_active);
    const int x = s.overview.x + 5, y = s.overview.y + 5;
    f.Input(0, KEY_TOUCH, x,
            y); // A gesture started outside the preview cannot become a pan.
    assert(s.viewport.center_x == 0.5f && s.viewport.center_y == 0.5f);
    f.Input(0);
    f.Input(KEY_TOUCH, KEY_TOUCH, x, y);
    assert(s.viewport.center_x < 0.5f && s.viewport.center_y < 0.5f &&
           s.viewport.interaction_active);
    qualities.clear();
    f.Input(0);
    assert(!s.viewport.interaction_active && !qualities.empty() &&
           qualities.front());
    f.Input(KEY_DRIGHT, KEY_DRIGHT);
    qualities.clear();
    f.Input(0);
    assert(!s.viewport.interaction_active && !qualities.empty() &&
           qualities.front());
    f.Input(KEY_TOUCH, KEY_TOUCH, x, y);
    f.reader.SuspendImageViewer();
    s.image.width = s.image.height = s.image.original_width =
        s.image.original_height = 1000;
    s.image.pixels.reset(new u16[1000 * 1000]());
    s.loading = s.suspended = false;
    f.Input(0); // Release gate after HOME.
    s.viewport.center_x = s.viewport.center_y = 0.5f;
    f.Input(0, KEY_TOUCH, x, y); // A stale gesture must not be resumed.
    assert(s.viewport.center_x == 0.5f && s.viewport.center_y == 0.5f);
  }
}
int main(int argc, char **argv) {
  if (argc == 1 || strcmp(argv[1], "layout") == 0)
    TestLayout();
  if (argc == 1 || strcmp(argv[1], "actions") == 0)
    TestActions();
  if (argc == 1 || strcmp(argv[1], "unavailable") == 0)
    TestUnavailableControls();
  if (argc == 1 || strcmp(argv[1], "touch") == 0)
    TestTouchOwnership();
  puts("PASS: image viewer controls, bounded layout and touch ownership");
}

#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
#include "app/app_mode.h"
#include "app/status_controller.h"
#include "app/status_layout_utils.h"
#include "shared/app_flow_utils.h"
#include "shared/orientation_utils.h"
#include "test_assert.h"

static const int TEXT_STYLE_BROWSER = 1;

struct Page {
  enum { IMAGE = 1 };
  struct Selectable { int kind, href_id; };
  std::vector<Selectable> items;
  const std::vector<Selectable> &GetRenderedSelectables() const { return items; }
};

struct Book {
  bool fixed = false;
  u8 orientation = orientation_utils::ORIENT_LANDSCAPE;
  Page page;
  bool IsPdf() const { return fixed; }
  bool IsCbz() const { return false; }
  bool IsFixedLayout() const { return fixed; }
  bool HasReadingPaceEstimate() const { return false; }
  int EstimateRemainingBookMinutes() const { return -1; }
  int EstimateRemainingChapterMinutes() const { return -1; }
  int GetPageCount() const { return 10; }
  int GetPosition() const { return 2; }
  int GetFocusedInlineLinkIndex() const { return -1; }
  u8 GetOrientation() const { return orientation; }
  Page *GetPage() { return &page; }
};

struct Text {
  struct Printed { std::string value; u16 *screen; int x, y; };
  struct Cleared { u16 *screen; int x0, y0, x1, y1; };
  struct Margins { int bottom = 36; } margin;
  u16 left{}, right{};
  u16 *screenleft = &left, *screenright = &right, *screen = screenleft;
  u8 orientation = orientation_utils::ORIENT_LANDSCAPE;
  int style = 7, x = 0, y = 0;
  std::vector<Printed> printed;
  std::vector<Cleared> cleared;
  u8 GetOrientation() const { return orientation; }
  u16 *GetScreen() const { return screen; }
  void SetScreen(u16 *value) { screen = value; }
  int GetStyle() const { return style; }
  void SetStyle(int value) { style = value; }
  int GetHeight() const { return 12; }
  int LogicalWidth() const {
    return orientation_utils::IsLandscape(orientation)
        ? (screen == screenleft ? 400 : 320) : 240;
  }
  int LogicalHeight() const {
    return orientation_utils::IsLandscape(orientation)
        ? 240 : (screen == screenleft ? 400 : 320);
  }
  int GetStringWidth(const char *value, int) const { return std::strlen(value) * 6; }
  u16 GetFgColor() const { return 0; }
  void SetPen(int px, int py) { x = px; y = py; }
  void PrintString(const char *value) {
    printed.push_back(Printed{value, screen, x, y});
  }
  void ClearRect(int x0, int y0, int x1, int y1) {
    cleared.push_back(Cleared{screen, x0, y0, x1, y1});
  }
  void DrawRect(int, int, int, int, u16) {}
  void FillRect(int, int, int, int, u16) {}
};

struct Prefs { bool show_time_remaining = false, time24h = true; };
struct App {
  std::unique_ptr<Text> ts{new Text};
  Prefs settings;
  Prefs *prefs = &settings;
  Book *book = nullptr;
  AppMode mode = AppMode::Opening;
  AppMode GetMode() const { return mode; }
  Book *GetCurrentBook() const { return book; }
  bool IsInlineLinkFocusActive() const { return false; }
};

namespace battery_utils {
bool ReadBatteryState(u8 *, bool *, bool *) { return false; }
void FormatBatteryString(char *, size_t, u8, bool, bool) {}
}

static const char *kOpeningStatusLabel = "opening";
#include "status_orientation_under_test.inc"
namespace app_flow_utils {
#include "status_snapshot_under_test.inc"
}

static void CheckOpening(u8 orientation) {
  App app;
  auto &text = *app.ts;
  text.orientation = orientation;
  StatusController status(app);
  status.UpdateStatus();
  const bool landscape = orientation_utils::IsLandscape(orientation);
  const int width = landscape ? 320 : 240;
  const int height = landscape ? 240 : 400;
  u16 *expected_screen = landscape ? text.screenright : text.screenleft;
  test::ExpectEq("opening draws one HUD strip", text.cleared.size(), 1);
  for (const auto &rect : text.cleared) {
    test::ExpectTrue("opening HUD uses the active screen", rect.screen == expected_screen);
    test::ExpectTrue("opening HUD clearing stays within logical screen",
                     rect.x0 >= 0 && rect.y0 >= 0 && rect.x1 <= width && rect.y1 <= height);
  }
  test::ExpectEq("opening prints clock and state", text.printed.size(), 2);
  bool found_opening = false;
  for (const auto &line : text.printed) {
    test::ExpectTrue("opening text uses the active screen", line.screen == expected_screen);
    test::ExpectTrue("opening text is visible within logical screen",
                     line.x >= 0 && line.x + (int)line.value.size() * 6 <= width &&
                         line.y - text.GetHeight() >= 0 && line.y <= height);
    found_opening |= line.value == "opening";
  }
  test::ExpectTrue("opening state remains visible without current book", found_opening);
  test::ExpectTrue("HUD restores screen", text.screen == text.screenleft);
  test::ExpectEq("HUD restores style", text.style, 7);
  test::ExpectEq("HUD restores margin", text.margin.bottom, 36);
}

int main() {
  CheckOpening(orientation_utils::ORIENT_LANDSCAPE);
  CheckOpening(orientation_utils::ORIENT_TURNED_LEFT);
  CheckOpening(orientation_utils::ORIENT_TURNED_RIGHT);
  App app;
  Book fixed;
  fixed.fixed = true;
  app.book = &fixed;
  app.mode = AppMode::Book;
  StatusController status(app);
  status.UpdateStatus();
  test::ExpectEq("fixed-layout book retains clock and page HUD", app.ts->cleared.size(), 2);
  test::ExpectStrEq("fixed-layout book retains page counter",
                     app.ts->printed.back().value.c_str(), "3/10");
  return 0;
}

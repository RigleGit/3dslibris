#include <3ds.h>
#include "app/frame_input.h"
#include "app/key_map.h"
#include "app/app_mode.h"
#include "settings/prefs_button_ids.h"
#include "settings/prefs_input_utils.h"
#include "shared/orientation_utils.h"
#include "test_assert.h"

static const u32 KEY_TOUCH = 1u << 20;
static const u32 KEY_A = 1u << 12, KEY_B = 1u << 13, KEY_Y = 1u << 15;
static const u32 KEY_START = 1u << 16, KEY_SELECT = 1u << 17;
static const u32 kGoToPageCoarseStep = 10;
#define DBG_LOGF(...) ((void)0)

struct Text {
  u8 orientation = orientation_utils::ORIENT_TURNED_LEFT;
  u8 GetOrientation() const { return orientation; }
};

enum Action { NONE, NEXT_ITEM, PREVIOUS_ITEM, NEXT_PAGE, PREVIOUS_PAGE,
              ACTIVATE, BACK, TOUCH };

// The handlers under test own the key-to-action dispatch. The navigation and
// graphics collaborators record its observable action without emulating it.
struct MenuFixture {
  Text text;
  Text *ts = &text;
  struct Context {
    KeyMap keys = {};
    bool IsBookSettingsContext() const { return false; }
    void ShowSettingsView(bool) {}
  } context_;
  Action action = NONE;
  void *status_reporter = nullptr;
  bool wait_input_release = false;
  u64 wait_input_release_started_ms = 0;
  void ActivateSelected() { action = ACTIVATE; }
  void Back() { action = BACK; }
  void SelectNext() { action = NEXT_ITEM; }
  void SelectPrevious() { action = PREVIOUS_ITEM; }
  void NextPage() { action = NEXT_PAGE; }
  void PreviousPage() { action = PREVIOUS_PAGE; }
  void HandleTouchInput(const FrameInput &) { action = TOUCH; }
};

struct PagedListMenu : MenuFixture {
  void HandleInput(const FrameInput &input);
};

struct FontMenu : MenuFixture {
  void handleTargetInput(const FrameInput &input);
  void handleFileInput(const FrameInput &input);
  void enterFileView() { action = ACTIVATE; }
  void enterTargetView(u8) { action = BACK; }
  void handleButtonPress() { action = ACTIVATE; }
  void nextTargetPage() { action = NEXT_PAGE; }
  void previousTargetPage() { action = PREVIOUS_PAGE; }
  void selectNextTarget() { action = NEXT_ITEM; }
  void selectPreviousTarget() { action = PREVIOUS_ITEM; }
  void selectNext() { action = NEXT_ITEM; }
  void selectPrevious() { action = PREVIOUS_ITEM; }
  void nextPage() { action = NEXT_PAGE; }
  void previousPage() { action = PREVIOUS_PAGE; }
  void handleTargetTouchInput(const FrameInput &) { action = TOUCH; }
  void handleFileTouchInput(const FrameInput &) { action = TOUCH; }
  u8 targetSelected = 0;
};

struct App {
  KeyMap key = {};
  u8 render_orientation = orientation_utils::ORIENT_TURNED_LEFT;
  int selected = 1;
  int GetPrefsSelectedIndex() const { return selected; }
  void SetPrefsSelectedIndex(int value) { selected = value; }
  bool IsBookSettingsContext() const { return false; }
  bool IsPrefsDirty() const { return false; }
  AppMode GetMode() const { return AppMode::Prefs; }
  void *GetCurrentBook() const { return nullptr; }
  void MarkPrefsDirty() {}
  void ShowLibraryView() {}
  void ReturnFromPrefs() {}
};

static bool CurrentBookHasExtraPrefsPage(void *, bool) { return false; }

struct SettingsController {
  App app_;
  int prefs_general_page_ = 0, value_delta = 0;
  int selected_button = PREFS_BUTTON_FONTSIZE;
  struct Dialog {
    bool open = false;
    int target = 100;
    bool IsOpen() const { return open; }
    void Confirm() {}
    void Close() { open = false; }
    void AdjustTarget(int delta) { target += delta; }
    void HandleTouch(const FrameInput &, bool) {}
  } go_to_page_dialog_;
  u8 PrefsVisibleButtonCount() const { return 3; }
  int EffectiveButtonForSlot(int) const { return selected_button; }
  void PrefsHandleEvent(const FrameInput &input);
  void GoToPrefsPage(int value) { prefs_general_page_ = value; }
  void PrefsDraw() {}
  void PrefsHandlePress() {}
  void PrefsHandleTouch(const FrameInput &) {}
  void PrefsDecreasePixelSize() { --value_delta; }
  void PrefsIncreasePixelSize() { ++value_delta; }
  void PrefsDecreaseLineSpacing() { --value_delta; }
  void PrefsIncreaseLineSpacing() { ++value_delta; }
  void PrefsDecreaseParaspacing() { --value_delta; }
  void PrefsIncreaseParaspacing() { ++value_delta; }
};

#include "menu_orientation_under_test.inc"

static KeyMap PhysicalKeys() {
  KeyMap keys = {};
  u32 bit = 1;
  u32 *fields[] = {&keys.up, &keys.down, &keys.left, &keys.right,
                   &keys.zl, &keys.zr, &keys.l, &keys.r, &keys.dup,
                   &keys.ddown, &keys.dleft, &keys.dright, &keys.a,
                   &keys.b, &keys.x, &keys.y, &keys.start, &keys.select};
  for (u32 *field : fields) {
    *field = bit;
    bit <<= 1;
  }
  return keys;
}

template <class Menu>
static void CheckInput(Menu &menu, void (Menu::*handler)(const FrameInput &),
                       u32 keys, Action expected) {
  menu.action = NONE;
  (menu.*handler)(FrameInput(keys, 0, false, 0, 0, 1000));
  test::ExpectEq("menu follows visible direction", menu.action, expected);
}

template <class Menu>
static void CheckDirections(Menu &menu,
                            void (Menu::*handler)(const FrameInput &)) {
  const KeyMap key = PhysicalKeys();
  menu.context_.keys = key;
  for (u8 orientation : {orientation_utils::ORIENT_TURNED_LEFT,
                         orientation_utils::ORIENT_TURNED_RIGHT}) {
    menu.text.orientation = orientation;
    const bool right = orientation_utils::IsTurnedRight(orientation);
    const u32 next[] = {right ? key.up : key.down,
                        right ? key.dup : key.ddown,
                        right ? key.right : key.left,
                        right ? key.dright : key.dleft};
    const u32 previous[] = {right ? key.down : key.up,
                            right ? key.ddown : key.dup,
                            right ? key.left : key.right,
                            right ? key.dleft : key.dright};
    for (u32 input : next)
      CheckInput(menu, handler, input, NEXT_ITEM);
    for (u32 input : previous)
      CheckInput(menu, handler, input, PREVIOUS_ITEM);
    CheckInput(menu, handler, right ? key.l : key.r, NEXT_PAGE);
    CheckInput(menu, handler, right ? key.r : key.l, PREVIOUS_PAGE);
    CheckInput(menu, handler, key.a, ACTIVATE);
    CheckInput(menu, handler, KEY_TOUCH, TOUCH);
    CheckInput(menu, handler, 0, NONE);
  }
}

int main() {
  PagedListMenu list;
  CheckDirections(list, &PagedListMenu::HandleInput);
  FontMenu fonts;
  CheckDirections(fonts, &FontMenu::handleTargetInput);
  CheckDirections(fonts, &FontMenu::handleFileInput);
  const KeyMap key = PhysicalKeys();
  for (u8 orientation : {orientation_utils::ORIENT_TURNED_LEFT,
                         orientation_utils::ORIENT_TURNED_RIGHT}) {
    SettingsController settings;
    settings.app_.render_orientation = orientation;
    settings.app_.key = key;
    const bool right = orientation_utils::IsTurnedRight(orientation);
    if (right)
      std::swap(settings.app_.key.l, settings.app_.key.r);
    for (u32 next : {right ? key.right : key.left,
                     right ? key.dright : key.dleft, settings.app_.key.r}) {
      settings.app_.selected = 1;
      settings.PrefsHandleEvent(FrameInput(next, 0, false, 0, 0, 1000));
      test::ExpectEq("settings selection moves down", settings.app_.selected, 2);
    }
    for (u32 previous : {right ? key.left : key.right,
                         right ? key.dleft : key.dright, settings.app_.key.l}) {
      settings.app_.selected = 1;
      settings.PrefsHandleEvent(FrameInput(previous, 0, false, 0, 0, 1000));
      test::ExpectEq("settings selection moves up", settings.app_.selected, 0);
    }
    for (int button : {PREFS_BUTTON_FONTSIZE, PREFS_BUTTON_LINE_SPACING,
                        PREFS_BUTTON_PARASPACING}) {
      settings.selected_button = button;
      settings.value_delta = 0;
      settings.PrefsHandleEvent(FrameInput(right ? key.up : key.down,
                                           0, false, 0, 0, 1000));
      test::ExpectEq("settings value increases to the right", settings.value_delta, 1);
      settings.PrefsHandleEvent(FrameInput(right ? key.down : key.up,
                                           0, false, 0, 0, 1000));
      test::ExpectEq("settings value decreases to the left", settings.value_delta, 0);
    }
  }
  return 0;
}

#pragma once

#include "ui/button.h"
#include "menus/go_to_page_dialog.h"

class App;
struct FrameInput;

class SettingsController {
public:
  explicit SettingsController(App &app);

  void ShowSettingsView(bool from_book);
  void ToggleCurrentBookMobiLineWrapFix();
  unsigned char PrefsVisibleButtonCount() const;
  void PrefsInit();
  void PrefsDraw();
  void PrefsHandleEvent(const FrameInput &input);
  void PrefsHandlePress();
  void PrefsHandleTouch(const FrameInput &input);
  void PrefsIncreasePixelSize();
  void PrefsDecreasePixelSize();
  void PrefsIncreaseLineSpacing();
  void PrefsDecreaseLineSpacing();
  void PrefsIncreaseParaspacing();
  void PrefsDecreaseParaspacing();
  void PrefsFlipOrientation();
  void PrefsToggleHandedness();
  void PrefsRefreshButton(int index);

private:
  App &app_;
  GoToPageDialog go_to_page_dialog_;
  int prefs_general_page_;
  Button button_prefs_page_nav_;
  Button button_prefs_library_;  // "library" button shown in book context

  void ResetToDefaults();
  void ClearAllCaches();
  int EffectiveVisibleCount() const;
  int EffectiveButtonForSlot(int slot) const;
  void GoToPrefsPage(int page, bool select_last = false);
};

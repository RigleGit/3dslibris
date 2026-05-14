#pragma once

#include "ui/button.h"
#include "menus/go_to_page_dialog.h"

class App;

class SettingsController {
public:
  explicit SettingsController(App &app);

  void ShowSettingsView(bool from_book);
  void ToggleCurrentBookMobiLineWrapFix();
  unsigned char PrefsVisibleButtonCount() const;
  void PrefsInit();
  void PrefsDraw();
  void PrefsHandleEvent();
  void PrefsHandlePress();
  void PrefsHandleTouch();
  void PrefsIncreasePixelSize();
  void PrefsDecreasePixelSize();
  void PrefsIncreaseParaspacing();
  void PrefsDecreaseParaspacing();
  void PrefsFlipOrientation();
  void PrefsRefreshButton(int index);

private:
  App &app_;
  GoToPageDialog go_to_page_dialog_;
  int prefs_general_page_;
  Button button_prefs_page_nav_;

  void ResetToDefaults();
  void ClearAllCaches();
  int EffectiveVisibleCount() const;
  int EffectiveButtonForSlot(int slot) const;
  void GoToPrefsPage(int page);
};

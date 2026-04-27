#pragma once

#include "ui/button.h"

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
  bool go_to_page_popup_open_;
  int go_to_page_target_page_;
  int prefs_general_page_;
  Button button_prefs_page_nav_;

  void OpenGoToPagePopup();
  void CloseGoToPagePopup();
  bool IsGoToPagePopupOpen() const;
  void AdjustGoToPageTarget(int delta);
  bool ConfirmGoToPageSelection();
  void DrawGoToPagePopup();
  void HandleGoToPagePopupTouch(bool touch_down);
  void ResetToDefaults();
  void ClearAllCaches();
  int EffectiveVisibleCount() const;
  int EffectiveButtonForSlot(int slot) const;
  void GoToPrefsPage(int page);
};

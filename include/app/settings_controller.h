#pragma once

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
};

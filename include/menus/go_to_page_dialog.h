/*
    3dslibris - go_to_page_dialog.h
    New 3DS menu module by Rigle.

    Summary:
    - Self-contained overlay dialog for jumping to a specific page in the current book.
    - Renders a slider + cancel/go buttons; handles touch and d-pad input.
*/

#pragma once

#include <3ds.h>

class App;

class GoToPageDialog {
public:
  explicit GoToPageDialog(App &app);

  void Open();
  void Close();
  bool IsOpen() const;
  void AdjustTarget(int delta);
  bool Confirm();
  void Draw();
  void HandleTouch(bool touch_down);

private:
  App &app_;
  bool open_;
  int target_page_;
};

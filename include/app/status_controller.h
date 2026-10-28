#pragma once

#include <stdint.h>

class Book;
class App;

class StatusController {
public:
  explicit StatusController(App &app);

  void RequestStatusRedraw();
  void UpdateStatus();

private:
  App &app_;
  int last_minute_;
  int last_display_token_;
  Book *progress_lock_book_;
  int progress_pagecount_lock_;
  bool force_redraw_;
  uint64_t battery_checked_ms_;
  unsigned char battery_percent_;
  bool battery_sampled_, battery_valid_, battery_charging_, battery_approx_;
};

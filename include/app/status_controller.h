#pragma once

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
};

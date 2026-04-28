#pragma once

#include <deque>

#include "app/app.h"

class Book;
class Text;

struct LibraryGradientContext {
  Text *ts;
  const u8 *color_mode;
};

class LibraryController {
public:
  explicit LibraryController(App &app);

  int FindBooks();
  void PrepareLibrary();
  void browser_draw();
  void browser_handleevent();
  void browser_init();
  void UnloadNonVisibleBrowserCoverCaches();
  void browser_nextpage();
  void browser_prevpage();
  void LoadVisibleBrowserCoverCaches();
  void PrioritizeSelectedBookJobs(Book *selected_book);
  bool HasQueuedJob(app_job_type_t type, Book *book) const;
  void EnqueueJob(app_job_type_t type, Book *book);
  void TickBrowserWarmup();
  void browser_tick_marquee();
  void ResetBrowserMarquee();
  void QueueBookWarmup(Book *book);
  void QueueTocResolve(Book *book);
  void ProcessJobs(u32 budget_ms);
  size_t PauseBrowserJobs();
  bool IsInsideFolder() const;

private:
  App &app_;
  std::deque<app_job_t> job_queue_;
  LibraryGradientContext gradient_ctx_;
  bool inside_folder_;
  std::string current_folder_name_;
  std::string current_folder_path_;

  void RebuildRoot();
  void EnterFolder(Book *folder);
  void LeaveFolder();
  void OpenSelectedBrowserEntry();
};

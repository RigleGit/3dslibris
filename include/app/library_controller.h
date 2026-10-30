#pragma once

#include <3ds/types.h>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>
#include <memory>

#include "library/library_sort_mode.h"

#include "library/library_job.h"

class App;
struct FrameInput;
class Book;
class Text;
class Button;
struct LibrarySearchSession;
struct LibraryPreloadSession;
namespace library_preload { struct Result; }
namespace library_walk { struct Entry; }
namespace library_search { struct MetadataValue; enum class MetadataStatus; }

struct LibraryGradientContext {
  Text *ts;
  const u8 *color_mode;
};

class LibraryController {
public:
  explicit LibraryController(App &app);
  ~LibraryController();

  int FindBooks();
  void PrepareLibrary();
  void SortBooks();
  void browser_draw();
  void browser_handleevent(const FrameInput &input);
  void browser_init();
  void LayoutBrowserNavButtons();
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
  Book *RestoreSavedBookSelection(const char *folder, const char *filename);
  void OpenLibraryPreload();
  void RunLibraryPreloadFrame(const FrameInput &input);
  void ResumeLibraryPreload();
  void OpenLibrarySearch();
  void RunLibrarySearchFrame(const FrameInput &input);
  void ResumeLibrarySearch();

private:
  App &app_;
  std::deque<app_job_t> job_queue_;
  LibraryGradientContext gradient_ctx_;
  bool inside_folder_;
  std::string current_folder_name_;
  std::string current_folder_path_;
  std::vector<Book *> root_books_;
  std::vector<Button *> root_buttons_;
  Book *root_selected_book_;
  LibrarySortMode root_sort_mode_;
  bool root_cached_;
  bool browser_assets_pending_;
  std::unique_ptr<LibrarySearchSession> search_;
  std::unique_ptr<LibraryPreloadSession> preload_;
  void DrawLibraryPreload();
  void CloseLibraryPreload();
  Book *PreloadBook(const library_walk::Entry &);
  static bool PreloadRoot(void *, size_t, library_walk::Entry *);
  static bool PreloadCancelled(void *);
  static library_preload::Result PreloadMetadata(void *, const library_walk::Entry &);
  static library_preload::Result PreloadCover(void *, const library_walk::Entry &);
  void DrawLibrarySearch();
  void EditLibrarySearch();
  void StartLibrarySearch();
  void CloseLibrarySearch();
  void RestoreSearchOrigin();
  void ActivateLibrarySearchResult(size_t index);
  static bool SearchRoot(void *, size_t, library_walk::Entry *);
  static library_search::MetadataStatus SearchMetadata(void *, const std::string &, library_search::MetadataValue *);

  void RebuildRoot();
  void EnterFolder(Book *folder);
  void LoadFolderPath(const std::string &folder_path,
                      const std::string &folder_name);
  void LeaveFolder();
  void OpenSelectedBrowserEntry();
};

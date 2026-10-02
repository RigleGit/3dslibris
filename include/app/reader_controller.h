#pragma once

#include <stdint.h>
#include <memory>
struct ReaderSearchSession;

class App;
struct FrameInput;
class Book;

class ReaderController {
public:
  explicit ReaderController(App &app);
  ~ReaderController();
  void OpenSearch();
  void RunSearchFrame(const FrameInput &input);
  void InvalidateSearch(bool forget_query);

  void CloseBook();
  int GetBookIndex(Book *book);
  void HandleEventInBook(const FrameInput &input);
  void HandleEventInOpening(const FrameInput &input);
  unsigned char OpenBook();
  void ToggleBookmark();
  void OnAppletSuspendRequested();
  void OnAppletSuspended();
  void OnAppletResumed();

private:
  void ClearDeferredRelayoutState();
  void DrawSearch();
  void StartSearch(bool backwards, bool first);
  void EditSearchText();
  std::unique_ptr<ReaderSearchSession> search_;
  bool MaybeFinalizeDeferredRelayout(Book *book, int page_count);
  void MarkProgressDirty(Book *book);
  void TryPersistProgress(Book *book, bool force);
  void ResetProgressAutosave(Book *book);

  App &app_;
  Book *progress_autosave_book_;
  bool progress_autosave_dirty_;
  uint64_t last_progress_persist_ms_;
};

#pragma once

#include <stdint.h>

class App;
class Book;

class ReaderController {
public:
  explicit ReaderController(App &app);

  void CloseBook();
  int GetBookIndex(Book *book);
  void HandleEventInBook();
  void HandleEventInOpening();
  unsigned char OpenBook();
  void ToggleBookmark();
  void OnAppletSuspendRequested();
  void OnAppletSuspended();
  void OnAppletResumed();

private:
  void ClearDeferredRelayoutState();
  bool MaybeFinalizeDeferredRelayout(Book *book, int page_count);
  void MarkProgressDirty(Book *book);
  void TryPersistProgress(Book *book, bool force);
  void ResetProgressAutosave(Book *book);

  App &app_;
  Book *progress_autosave_book_;
  bool progress_autosave_dirty_;
  uint64_t last_progress_persist_ms_;
};

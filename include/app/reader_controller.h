#pragma once

#include <stdint.h>
#include <memory>
struct ReaderSearchSession;
struct ReaderImageViewerSession;

class App;
struct FrameInput;
class Book;

class ReaderController {
public:
  explicit ReaderController(App &app);
  ~ReaderController();
  void OpenImageViewer();
  void RunImageViewerFrame(const FrameInput &input);
  void InvalidateImageViewer();
  void SuspendImageViewer();
  void OpenSearch();
  void RunSearchFrame(const FrameInput &input);
  void InvalidateSearch(bool forget_query);

  void CloseBook(bool persist_progress = true);
  int GetBookIndex(Book *book);
  void HandleEventInBook(const FrameInput &input);
  void HandleEventInOpening(const FrameInput &input);
  unsigned char OpenBook();
  void ToggleBookmark();
  void OnAppletSuspendRequested();
  void OnAppletSuspended();
  void OnAppletResumed();

private:
  void DrawImageViewer();
  bool LoadImageViewer(int zoom);
  void CloseImageViewer();
  bool ImageViewerValid() const;
  std::unique_ptr<ReaderImageViewerSession> image_viewer_;
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

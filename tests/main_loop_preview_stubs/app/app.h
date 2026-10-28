#pragma once
#include <3ds.h>
#include <cstdlib>
#include <memory>
#include <vector>
#include "book/book.h"
#include "settings/prefs.h"
#include "formats/common/fixed_layout_preview_panel.h"

#include "app/app_mode.h"
#include "app/frame_input.h"

// Platform/App substitute: rendering still uses the actual preview panel, and
// RunMainLoop owns the ordering between input, HUD writes and presentation.
class App {
  AppMode mode_;
  bool status_dirty_;
  Book book_;
  fixed_layout_preview_panel::State panel_;
  std::vector<u16> preview_;
public:
  std::unique_ptr<Text> ts;
  std::unique_ptr<Prefs> prefs;
  int pans, presentations, visible_huds;
  bool simulate_pan = true;
  static const u16 kHudPixel = 0x5a5a;
  App() : mode_(AppMode::Book), status_dirty_(true), book_(NULL),
      preview_(7 * 11), ts(new Text(64, 64)), prefs(new Prefs),
      pans(0), presentations(0), visible_huds(0) {
    book_.text = ts.get();
    for (size_t i = 0; i < preview_.size(); ++i)
      preview_[i] = (u16)(i * 1739 + 27);
  }
  int BackgroundDraws() const { return book_.background_draws; }
  AppMode GetMode() const { return mode_; }
  void SetMode(AppMode mode) { mode_ = mode; }
  bool IsAppletExitRequested() const { return false; }
  bool IsAppletSuspended() const { return false; }
  void HandleAppletSuspend() { std::abort(); }
  void HandleAppletResume() {}
  bool HasPendingBootReopen() const { return false; }
  void SetPendingBootReopen(bool) { std::abort(); }
  void OpenBook() { std::abort(); }
  void UpdateStatus() {
    if (GetMode() != AppMode::Book && GetMode() != AppMode::Opening) return;
    if (!status_dirty_) return;
    ts->SetScreen(ts->screenright);
    // Representative clock/page HUD write outside the preview: it dirties the
    // same logical bottom screen and must remain visible after presentation.
    ts->FillRect(52, 52, 60, 60, kHudPixel);
    status_dirty_ = false;
  }
  void HandleEventInBook(const FrameInput &) {
    if (!simulate_pan) return;
    const pdf_view_utils::PreviewLayout layout = {4, 6, 23, 31};
    const fixed_layout_preview_panel::Rect viewport = {5 + pans, 8, 9, 12};
    fixed_layout_preview_panel::Draw(&book_, ts.get(), &panel_,
        preview_.data(), preview_.size(), 7, 11, 2, 3, false, layout, viewport);
    ++pans;
    // ReaderController requests the HUD after each successful fixed-layout pan.
    status_dirty_ = true;
  }
  void PresentIfDirty() {
    if (!ts->HasDirtyScreens()) return;
    ++presentations;
    if (ts->bottom[55 * ts->stride + 55] == kHudPixel) ++visible_huds;
    ts->Present();
  }
  bool ShouldAbortWork() const { return false; }
  Book *GetCurrentBook() { return &book_; }
  // Every non-book branch must stay inactive in this focused reader scenario.
  void HandleEventInOpening(const FrameInput &) { std::abort(); }
  void browser_handleevent(const FrameInput &) { std::abort(); }
  void TickBrowserWarmup() { std::abort(); }
  bool IsBrowserDirty() const { return false; }
  void browser_draw() { std::abort(); }
  void browser_tick_marquee() { std::abort(); }
  void PrefsHandleEvent(const FrameInput &) { std::abort(); }
  bool IsPrefsDirty() const { return false; }
  void PrefsDraw() { std::abort(); }
  void RunFontMenuFrame(const FrameInput &) { std::abort(); }
  void RunBookmarksMenuFrame(const FrameInput &) { std::abort(); }
  void RunChaptersMenuFrame(const FrameInput &) { std::abort(); }
  void RunImageViewerFrame(const FrameInput &) { std::abort(); }
  void RunSearchFrame(const FrameInput &) { std::abort(); }
  void RunBookInfoFrame(const FrameInput &) { std::abort(); }
  bool ShouldSkipNextBrowserPresent() const { return false; }
  void ClearSkipNextBrowserPresent() { std::abort(); }
  u64 GetBrowserLastInteractionMs() const { return 0; }
  bool IsBrowserWaitingInputRelease() const { return false; }
  void ProcessJobs(int) { std::abort(); }
};

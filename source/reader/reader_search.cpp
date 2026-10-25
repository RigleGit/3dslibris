#include "app/reader_controller.h"
#include "reader/search_session.h"
#include "app/app.h"
#include "app/frame_input.h"
#include "book/book_search_source.h"
#include "book/book_renderer.h"
#include "reader/book_page_nav.h"
#include "reader/reader_controls.h"
#include "shared/debug_log.h"
#include "shared/orientation_utils.h"
#include "shared/text_unicode_utils.h"
#include "ui/touch_utils.h"
#include <3ds.h>
#include <algorithm>

namespace {
std::string ShortLabel(const std::string &s, size_t chars) {
  const size_t bytes = text_unicode_utils::Utf8BytesForDisplayChars(s.c_str(), chars);
  return bytes < s.size() ? s.substr(0, bytes) + "..." : s;
}
bool MatchIncludesPage(const text_search::Match &match, unsigned page) {
  return page >= match.start.page &&
      (page < match.end.page || (page == match.end.page && match.end.token != 0));
}
void LogSearch(App &app, const ReaderSearchSession &s, const char *reason) {
  DBG_LOGF(&app,
      "SEARCH %s status=%d elapsed_ms=%llu scan_ms=%llu max_slice_ms=%llu tokens=%llu auxiliary_bytes=%lu pages=%u wrapped=%d rtl_excluded=%d",
      reason, (int)s.engine.GetStatus(),
      (unsigned long long)(osGetTime() - s.started_ms),
      (unsigned long long)s.scan_ms, (unsigned long long)s.max_slice_ms,
      (unsigned long long)s.engine.Scanned(), (unsigned long)s.engine.AuxiliaryBytes(),
      s.pages, (int)s.engine.Wrapped(), (int)s.engine.ExcludedRtl());
}
} // namespace

void ReaderController::InvalidateSearch(bool forget_query) {
  if (!search_) return;
  if (search_->engine.GetStatus() == text_search::Searching)
    LogSearch(app_, *search_, "cancel");
  search_->engine.Cancel();
  search_->active = search_->has_match = false;
  search_->book = nullptr;
  if (forget_query) search_.reset();
}

void ReaderController::OpenSearch() {
  InvalidateImageViewer();
  Book *book = app_.GetCurrentBook();
  if (!book || book->IsFixedLayout() || !book->GetPageCount() ||
      book->IsAsyncReflowOpenPending()) return;
  if (app_.BookNeedsRelayout(book)) {
    app_.ReturnFromPrefs();
    if (app_.GetMode() != AppMode::Book || book->IsAsyncReflowOpenPending()) return;
  }
  if (!search_) search_.reset(new ReaderSearchSession);
  ReaderSearchSession &s = *search_;
  s.book = book;
  s.session = book->GetOpenSessionId(); s.revision = book->GetLayoutRevision();
  s.pages = book->GetPageCount();
  s.active = s.editor = s.dirty = s.wait_release = true;
  s.has_match = false; s.message.clear(); s.selected = 0;
  s.keyboard_pending = s.query.empty(); s.release_ms = osGetTime();
  app_.SetInlineLinkFocusActive(false);
  book->ClearFocusedInlineLink();
  app_.ResetPageRepeat();
  for (int i = 0; i < 4; ++i) {
    s.buttons[i].Init(app_.ts.get());
    s.buttons[i].SetStyle(BUTTON_STYLE_SETTING);
    s.buttons[i].Move(5, i == 3 ? 286 : 65 + i*54);
    s.buttons[i].Resize(230, i == 3 ? 28 : 44);
  }
  app_.SetMode(AppMode::Search);
  DrawSearch();
}

void ReaderController::EditSearchText() {
  ReaderSearchSession &s = *search_;
  s.keyboard_pending = true;
  // Present the editor before transferring control to the system applet.
  DrawSearch(); app_.PresentIfDirty();
  if (app_.ShouldAbortWork()) return;
  s.keyboard_pending = false;
  SwkbdState keyboard;
  swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, 128);
  swkbdSetHintText(&keyboard, "Search in this book");
  swkbdSetInitialText(&keyboard, s.query.c_str());
  swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
  swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
  swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "OK", true);
  swkbdSetFeatures(&keyboard, SWKBD_PREDICTIVE_INPUT |
                             SWKBD_ALLOW_HOME | SWKBD_ALLOW_POWER);
  char query[128 * 4 + 1] = {};
  const SwkbdButton result = swkbdInputText(&keyboard, query, sizeof(query));
  if (result == SWKBD_BUTTON_RIGHT) {
    s.query = query; s.selected = 2; s.message.clear();
  } else if (result == SWKBD_BUTTON_NONE && swkbdGetResult(&keyboard) < 0) {
    s.message = "Keyboard unavailable; try again";
  }
  s.wait_release = s.dirty = true; s.release_ms = osGetTime();
  app_.ts->MarkAllScreensDirty();
  // The next main-loop frame owns restore/HOME handling and redraw.
}

void ReaderController::StartSearch(bool backwards, bool first) {
  ReaderSearchSession &s = *search_;
  if (s.engine.GetStatus() == text_search::Searching)
    LogSearch(app_, s, "cancel");
  s.engine.Cancel();
  if (!s.engine.Configure(s.query, s.whole_words)) {
    s.message = s.engine.GetError(); s.editor = s.dirty = true; return;
  }
  const unsigned page=s.book->GetPosition();
  // Keep the anchor while inspecting a match's continuation, but a manually
  // selected page outside that span becomes the new search origin.
  const bool resume_match=!first && s.has_match &&
      MatchIncludesPage(s.match, page);
  const text_search::Position anchor = resume_match ? s.match.start :
      text_search::Position(s.book->GetPosition(), 0);
  s.engine.Start(text_search::FromBook(s.book), anchor, backwards, !resume_match);
  if (!resume_match) s.has_match = false;
  if (s.engine.GetStatus() == text_search::Failed) {
    s.message = s.engine.GetError(); s.editor = s.dirty = true; return;
  }
  s.editor = false; s.message = "Searching... B: cancel"; s.dirty = true;
  s.started_ms = osGetTime(); s.scan_ms = s.max_slice_ms = 0;
  app_.ShowCurrentBookView(); app_.SetMode(AppMode::Search);
  DrawSearch();
}

void ReaderController::DrawSearch() {
  ReaderSearchSession &s = *search_;
  Text *ts = app_.ts.get();
  if (!s.active || app_.ShouldAbortWork()) return;
  if (s.editor) {
    app_.ApplyRenderOrientation(app_.portrait_orientation);
    ts->SetScreen(ts->screenright);
    app_.DrawBottomGradientBackground();
    const int old_style = ts->GetStyle();
    ts->SetStyle(TEXT_STYLE_BROWSER);
    ts->SetPen(8, 28); ts->PrintString("search");
    s.buttons[0].SetLabel1("word or phrase");
    s.buttons[0].SetLabel2(s.query.empty() ? "open keyboard >" : ShortLabel(s.query, 26));
    s.buttons[1].SetLabel1("whole words");
    s.buttons[1].SetLabel2(s.whole_words ? "on" : "off");
    s.buttons[2].SetLabel1("find"); s.buttons[2].SetLabel2("from current page >");
    s.buttons[3].Label("back");
    for (int i = 0; i < 4; ++i) s.buttons[i].Draw(i == s.selected);
    ts->SetPen(8, 250); ts->PrintString(ShortLabel(s.message, 31).c_str());
    ts->SetStyle(old_style); ts->MarkScreenDirty(ts->screenright);
  } else {
    app_.ApplyRenderOrientation(app_.reader_layout.orientation);
    Page *page = s.book->GetPage();
    Page::Highlight highlight{};
    const unsigned current = s.book->GetPosition();
    const bool on_match = s.has_match && MatchIncludesPage(s.match, current);
    if (on_match) {
      highlight.begin = current == s.match.start.page ? s.match.start.token : 0;
      highlight.end = current == s.match.end.page ? s.match.end.token : page->GetLength();
    }
    page->Draw(ts, on_match ? &highlight : nullptr);
    const bool landscape = orientation_utils::IsLandscape(s.book->GetOrientation());
    ts->SetScreen(landscape ? ts->screenright : ts->screenleft);
    const int height = ts->LogicalHeight();
    const int size = ts->GetPixelSize(), style = ts->GetStyle(), bottom = ts->margin.bottom;
    const bool wrap = ts->IsAutoWrapEnabled();
    ts->margin.bottom = 0; ts->SetStyle(TEXT_STYLE_BROWSER); ts->SetPixelSize(10);
    ts->SetAutoWrapEnabled(false);
    ts->ClearRect(0, height - 24, ts->LogicalWidth(), height);
    ts->SetPen(5, height - 14); ts->PrintString(ShortLabel(s.message, 38).c_str());
    ts->SetPen(5, height - 3); ts->PrintString("L/R find  X edit  A/B read");
    ts->SetAutoWrapEnabled(wrap); ts->SetPixelSize(size); ts->SetStyle(style); ts->margin.bottom = bottom;
    ts->MarkAllScreensDirty();
  }
  s.dirty = false;
}

void ReaderController::RunSearchFrame(const FrameInput &input) {
  if (!search_ || !search_->active) { app_.ShowCurrentBookView(); return; }
  ReaderSearchSession &s = *search_;
  Book *book = app_.GetCurrentBook();
  if (app_.ShouldAbortWork()) return;
  if (!book || book != s.book || book->GetOpenSessionId() != s.session ||
      book->GetLayoutRevision() != s.revision || book->GetPageCount() != s.pages ||
      book->IsAsyncReflowOpenPending()) {
    InvalidateSearch(false);
    if (book) { app_.ShowCurrentBookView(); book_nav::DrawPage(book, app_.ts.get()); }
    else app_.ShowLibraryView();
    return;
  }
  if (s.wait_release) {
    if (s.dirty) DrawSearch();
    if (input.keys_held && input.timestamp_ms - s.release_ms < 300) return;
    s.wait_release = false;
    return;
  }
  const u32 keys = input.keys_down;
  if (keys & app_.key.start) { app_.ShowLibraryView(); return; }
  if (keys & app_.key.select) { app_.ShowSettingsView(true); return; }
  if (s.editor) {
    if (s.keyboard_pending) { EditSearchText(); return; }
    bool activate = (keys & app_.key.a) != 0;
    if (keys & (app_.key.dup | app_.key.up)) { s.selected = (s.selected + 3) % 4; s.dirty = true; }
    if (keys & (app_.key.ddown | app_.key.down)) { s.selected = (s.selected + 1) % 4; s.dirty = true; }
    if (keys & KEY_TOUCH) {
      TouchCandidates points;
      touch::BuildCandidates(app_.MapTouch(input), &points);
      for (int i = 0; i < 4; ++i)
        if (touch::HitsButton(points, &s.buttons[i], 2)) { s.selected = i; activate = true; break; }
    }
    if ((keys & app_.key.b) || (activate && s.selected == 3)) {
      InvalidateSearch(false); app_.ShowCurrentBookView(); book_nav::DrawPage(book, app_.ts.get()); return;
    }
    if (activate) {
      if (s.selected == 0) { EditSearchText(); return; }
      if (s.selected == 1) { s.whole_words = !s.whole_words; s.dirty = true; }
      if (s.selected == 2) { StartSearch(false, true); return; }
    }
  } else {
    if ((keys & app_.key.b) && s.engine.GetStatus() == text_search::Searching) {
      LogSearch(app_, s, "cancel"); s.engine.Cancel(); s.message = "Search cancelled"; s.dirty = true;
    } else if (keys & (app_.key.a | app_.key.b)) {
      InvalidateSearch(false); app_.ShowCurrentBookView(); book_nav::DrawPage(book, app_.ts.get()); return;
    } else if (keys & app_.key.x) {
      if (s.engine.GetStatus() == text_search::Searching)
        LogSearch(app_, s, "cancel");
      s.engine.Cancel(); s.editor = s.dirty = true; s.selected = 0;
      s.message.clear();
    } else if (keys & (app_.key.l | app_.key.r)) {
      StartSearch((keys & app_.key.l) != 0, false);
    } else if (keys & (KEY_DLEFT | KEY_DRIGHT | KEY_DUP | KEY_DDOWN)) {
      const ReaderControls controls = orientation_utils::IsLandscape(book->GetOrientation()) ?
          BuildLandscapeControls(app_.key) : BuildPortraitControls(app_.key);
      const int delta = (keys & controls.page_next) ? 1 : (keys & controls.page_prev) ? -1 : 0;
      if (delta) {
        const bool cancelled=s.engine.GetStatus()==text_search::Searching;
        if (cancelled) LogSearch(app_, s, "cancel");
        s.engine.Cancel();
        u16 position = book->GetPosition();
        if (book_nav::TurnPage(book, app_.ts.get(), &position, book->GetPageCount(), delta, false)) {
          MarkProgressDirty(book); s.dirty = true;
        }
        s.message=cancelled ? "Search cancelled" :
            s.has_match && MatchIncludesPage(s.match, book->GetPosition()) ?
                "Match" : "L/R search from this page";
        s.dirty=true;
      }
    }
    if (s.engine.GetStatus() == text_search::Searching) {
      const uint64_t started = osGetTime();
      do { s.engine.Pump(32); }
      while (s.engine.GetStatus() == text_search::Searching && osGetTime() - started < 2);
      const uint64_t elapsed = osGetTime() - started;
      s.scan_ms += elapsed; s.max_slice_ms = std::max(s.max_slice_ms, elapsed);
      if (s.engine.GetStatus() != text_search::Searching) {
        LogSearch(app_, s, "complete");
        if (s.engine.GetStatus() == text_search::Found) {
          s.match = s.engine.GetMatch(); s.has_match = true;
          book_nav::SetPage(book, app_.ts.get(), s.match.start.page, false);
          book->ResetReadingPaceEstimate(); MarkProgressDirty(book);
          s.message = s.engine.Wrapped() ? "Match (wrapped)" : "Match";
        } else s.message = s.engine.GetStatus() == text_search::Failed ?
            s.engine.GetError() : "No other matches";
        if (s.engine.ExcludedRtl()) s.message += " | RTL excluded";
        s.dirty = true;
      }
    }
    TryPersistProgress(book, false);
  }
  if (s.dirty) DrawSearch();
}

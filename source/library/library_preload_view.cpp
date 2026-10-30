#include "app/library_controller.h"
#include "book/book.h"
#include "library/library_preload_session.h"
#include "library/cover_cache.h"
#include "library/cover_preparation.h"
#include "library/browser_view_utils.h"
#include "formats/common/library_metadata.h"
#include "formats/common/fixed_layout_gpu_renderer.h"
#include "app/app.h"
#include "shared/app_flow_utils.h"
#include "shared/debug_log.h"
#include "shared/text_unicode_utils.h"
#include "ui/text.h"
#include "settings/prefs.h"
#include <3ds.h>
#include <new>
#include <sys/stat.h>

namespace {
uint64_t PreloadClock() {
  return osGetTime();
}
std::string PreloadPath(Book *book) {
  return book->IsBrowserFolder()
             ? book->GetBrowserFolderPath()
             : std::string(book->GetFolderName()) + "/" + book->GetFileName();
}
std::string FitPreload(Text *ts, const std::string &text) {
  const unsigned chars =
      ts->GetCharCountInsideWidth(text.c_str(), TEXT_STYLE_BROWSER, 224);
  return text.substr(
      0, text_unicode_utils::Utf8BytesForDisplayChars(text.c_str(), chars));
}
void DrawCounts(Text *ts, const char *label,
                const library_preload::Counts &counts, int y) {
  char text[100];
  ts->SetPen(6, y);
  ts->PrintString(label);
  snprintf(text, sizeof(text), "%u cached / %u saved", (unsigned)counts.cached,
           (unsigned)counts.saved);
  ts->SetPen(6, y + 22);
  ts->PrintString(text);
  snprintf(text, sizeof(text), "%u absent / %u unsupported",
           (unsigned)counts.absent, (unsigned)counts.unsupported);
  ts->SetPen(6, y + 44);
  ts->PrintString(text);
  snprintf(text, sizeof(text), "%u errors", (unsigned)counts.errors);
  ts->SetPen(6, y + 66);
  ts->PrintString(text);
}
} // namespace

bool LibraryController::PreloadRoot(void *ctx, size_t index,
                                    library_walk::Entry *out) {
  LibraryController &self = *static_cast<LibraryController *>(ctx);
  const auto &roots = self.inside_folder_ && self.root_cached_
                          ? self.root_books_
                          : self.app_.books;
  if (index >= roots.size())
    return false;
  Book *book = roots[index];
  out->path = PreloadPath(book);
  out->directory = book->IsBrowserFolder();
  out->name = out->directory ? book->GetBrowserFolderDisplayName()
                             : book->GetFileName();
  return true;
}
bool LibraryController::PreloadCancelled(void *ctx) {
  return static_cast<LibraryController *>(ctx)->app_.ShouldAbortWork();
}
Book *LibraryController::PreloadBook(const library_walk::Entry &entry) {
  auto &s = *preload_;
  if (s.book && s.book_path == entry.path)
    return s.book.get();
  s.book.reset();
  s.book_path = entry.path;
  s.metadata_failed = false;
  BookContext ctx;
  ctx.text = app_.ts.get();
  ctx.prefs = app_.prefs.get();
  ctx.status_reporter = &app_;
  s.book.reset(new (std::nothrow) Book(ctx));
  if (!s.book)
    return nullptr;
  if (entry.directory)
    s.book->SetBrowserFolderEntry(entry.path, entry.name, "");
  else {
    s.book->SetFolderName(entry.path.substr(0, entry.path.find_last_of('/')));
    s.book->SetFileName(entry.name.c_str());
    s.book->format = app_flow_utils::DetectBookFormat(entry.name.c_str());
  }
  return s.book.get();
}
library_preload::Result
LibraryController::PreloadMetadata(void *ctx,
                                   const library_walk::Entry &entry) {
  using namespace library_preload;
  LibraryController &self = *static_cast<LibraryController *>(ctx);
  if (self.app_.ShouldAbortWork())
    return {Outcome::Cancelled, false};
  Book *book = self.PreloadBook(entry);
  if (!book)
    return {Outcome::Error, true};
  library_metadata::Value value;
  auto status = library_metadata::Prepare(entry.path, &value, PreloadCancelled,
                                          ctx, true);
  if (status == library_metadata::Status::NeedsPreparation) {
    self.DrawLibraryPreload();
    self.app_.PresentIfDirty();
    if (self.app_.ShouldAbortWork())
      return {Outcome::Cancelled, false};
    status =
        library_metadata::Prepare(entry.path, &value, PreloadCancelled, ctx);
  }
  book->metadataIndexTried = true;
  book->metadataIndexed = status == library_metadata::Status::Ready;
  book->coverImagePath = std::move(value.entry.cover_image_path);
  self.preload_->metadata_failed = status == library_metadata::Status::Error;
  if (status == library_metadata::Status::Cancelled)
    return {Outcome::Cancelled, value.extracted};
  if (status == library_metadata::Status::Unsupported)
    return {Outcome::Unsupported, value.extracted};
  if (status != library_metadata::Status::Ready || !value.cache_saved) {
    DBG_LOGF(&self.app_, "LIBPRELOAD metadata failed path=%s write=%u",
             entry.path.c_str(), value.cache_saved ? 0u : 1u);
    return {status == library_metadata::Status::Ready ? Outcome::WriteError
                                                      : Outcome::Error,
            value.extracted};
  }
  return {value.extracted ? Outcome::Saved : Outcome::Cached, value.extracted};
}
library_preload::Result
LibraryController::PreloadCover(void *ctx, const library_walk::Entry &entry) {
  using namespace library_preload;
  LibraryController &self = *static_cast<LibraryController *>(ctx);
  if (self.app_.ShouldAbortWork())
    return {Outcome::Cancelled, false};
  Book *book = self.PreloadBook(entry);
  if (!book)
    return {Outcome::Error, true};
  const std::string path =
      entry.directory ? book->GetBrowserFolderCoverPath() : entry.path;
  auto result = library_cover::Prepare(book, path, self.app_.IsNew3dsDevice(),
                                       false, true);
  if (result.status == library_cover::Status::NeedsPreparation) {
    self.DrawLibraryPreload();
    self.app_.PresentIfDirty();
    if (self.app_.ShouldAbortWork())
      return {Outcome::Cancelled, false};
    result =
        library_cover::Prepare(book, path, self.app_.IsNew3dsDevice(), false);
  }
  Outcome outcome = Outcome::Error;
  switch (result.status) {
  case library_cover::Status::Cached:
    outcome = Outcome::Cached;
    break;
  case library_cover::Status::Prepared:
    outcome = Outcome::Saved;
    break;
  case library_cover::Status::Absent:
    outcome = self.preload_->metadata_failed ? Outcome::Error : Outcome::Absent;
    break;
  case library_cover::Status::Unsupported:
    outcome = Outcome::Unsupported;
    break;
  case library_cover::Status::WriteError:
    outcome = Outcome::WriteError;
    break;
  case library_cover::Status::Cancelled:
    outcome = Outcome::Cancelled;
    break;
  default:
    break;
  }
  if (outcome == Outcome::Error || outcome == Outcome::WriteError)
    DBG_LOGF(&self.app_, "LIBPRELOAD cover failed path=%s status=%d rc=%d",
             entry.path.c_str(), (int)result.status, result.code);
  self.preload_->book.reset();
  self.preload_->book_path.clear();
  return {outcome, result.heavy};
}

void LibraryController::OpenLibraryPreload() {
  if (app_.ShouldAbortWork())
    return;
  PauseBrowserJobs();
  preload_.reset(new (std::nothrow) LibraryPreloadSession);
  if (!preload_) {
    app_.PrintStatus("Preload memory unavailable");
    return;
  }
  Text *ts = app_.ts.get();
  fixed_layout_gpu::Software(ts, true);
  ts->SetTopScreenWideMode(false);
  app_.ApplyRenderOrientation(app_.portrait_orientation);
  auto &button = preload_->action;
  button.Init(ts);
  button.SetStyle(BUTTON_STYLE_SETTING);
  button.Move(5, 282);
  button.Resize(230, 30);
  app_.SetMode(AppMode::LibraryPreload);
  DrawLibraryPreload();
}
void LibraryController::ResumeLibraryPreload() {
  if (!preload_ || app_.GetMode() != AppMode::LibraryPreload ||
      app_.ShouldAbortWork())
    return;
  auto &s = *preload_;
  s.engine.Cancel();
  s.book.reset();
  s.book_path.clear();
  s.start_pending = true;
  s.finished = false;
  s.wait_release = true;
  s.message.clear();
  app_.ApplyRenderOrientation(app_.portrait_orientation);
  DrawLibraryPreload();
}
void LibraryController::CloseLibraryPreload() {
  if (!preload_)
    return;
  preload_->engine.Cancel();
  preload_->book.reset();
  // Reload disk data only. Selection pointers survive sorting and no source
  // extraction is admitted while leaving this screen.
  const auto refresh = [](const std::vector<Book *> &books) {
    for (Book *book : books) {
      if (!book->IsBrowserFolder()) {
        book->metadataIndexTried = false;
        book->TryLoadMetadataFromCache();
        book->ClearBrowserDisplayNameCache();
      }
      delete[] book->coverPixels;
      book->coverPixels = nullptr;
      book->coverWidth = book->coverHeight = 0;
      book->coverAttempts = 0;
      book->coverRetryAfterMs = 0;
    }
  };
  refresh(app_.books);
  if (root_cached_) {
    refresh(root_books_);
    root_sort_mode_ = static_cast<LibrarySortMode>(-1);
  }
  struct stat st;
  if (inside_folder_ &&
      (stat(current_folder_path_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)))
    RebuildRoot();
  Book *selected = app_.GetSelectedBook();
  SortBooks();
  app_.SetSelectedBook(selected);
  const auto view =
      app_.prefs ? app_.prefs->browser_view_mode : BROWSER_VIEW_GALLERY;
  const int page_size = browser_view_utils::PageSize(view);
  const int index = app_.GetBookIndex(selected);
  app_.SetBrowserPageStart(index >= 0 ? index / page_size * page_size : 0);
  if (browser_view_utils::ShouldLoadCovers(view)) {
    const int first = app_.GetBrowserPageStart();
    for (int i = first; i < first + page_size && i < app_.BookCount(); ++i) {
      Book *book = app_.books[i];
      const auto path = book->IsBrowserFolder()
                            ? book->GetBrowserFolderCoverPath()
                            : PreloadPath(book);
      if (!path.empty())
        cover_cache::TryLoadCachedOnly(book, path);
    }
  }
  ResetBrowserMarquee();
  app_.SetBrowserDirty(true);
  preload_.reset();
  app_.ShowSettingsView(false);
}
void LibraryController::DrawLibraryPreload() {
  if (!preload_ || app_.ShouldAbortWork())
    return;
  auto &s = *preload_;
  Text *ts = app_.ts.get();
  const int style = ts->GetStyle(), size = ts->GetPixelSize(),
            bottom = ts->margin.bottom;
  const bool wrap = ts->IsAutoWrapEnabled();
  ts->SetStyle(TEXT_STYLE_BROWSER);
  ts->SetPixelSize(10);
  ts->SetAutoWrapEnabled(false);
  ts->margin.bottom = 0;
  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
  ts->SetPen(6, 24);
  ts->PrintString("preload book data");
  const auto status = s.engine.GetStatus();
  std::string message = s.message;
  if (message.empty()) {
    if (s.start_pending)
      message = "Preparing library...";
    else if (status == library_preload::Status::WriteError)
      message = "SD write failed; retry is possible";
    else if (status == library_preload::Status::Cancelled)
      message = "Cancelled; saved data kept";
    else if (status == library_preload::Status::Complete)
      message = "Finished";
    else
      message = "Preparing... B: cancel";
  }
  ts->SetPen(6, 56);
  ts->PrintString(FitPreload(ts, message).c_str());
  ts->SetPen(6, 88);
  ts->PrintString(FitPreload(ts, s.engine.Current().name).c_str());
  const char *phases[] = {"scanning library", "metadata", "cover"};
  ts->SetPen(6, 116);
  ts->PrintString(phases[(int)s.engine.GetPhase()]);
  char count[80];
  snprintf(count, sizeof(count), "%u books examined",
           (unsigned)s.engine.Examined());
  ts->SetPen(6, 148);
  ts->PrintString(count);
  snprintf(count, sizeof(count), "%u folder read errors",
           (unsigned)s.engine.DirectoryErrors());
  ts->SetPen(6, 176);
  ts->PrintString(count);
  ts->SetPen(6, 224);
  ts->PrintString("Covers stay on SD until cache clear");
  ts->SetPen(6, 248);
  ts->PrintString("Cancel between books / operations");
  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  DrawCounts(ts, "metadata", s.engine.MetadataCounts(), 24);
  DrawCounts(ts, "covers (including folders)", s.engine.CoverCounts(), 126);
  ts->SetPen(6, 230);
  ts->PrintString("EPUB PDF XPS MOBI FB2 CBZ");
  ts->SetPen(6, 252);
  ts->PrintString("Other formats: external covers");
  s.action.Label(s.finished ? "back" : "cancel");
  s.action.Draw(false);
  ts->margin.bottom = bottom;
  ts->SetAutoWrapEnabled(wrap);
  ts->SetPixelSize(size);
  ts->SetStyle(style);
  ts->MarkAllScreensDirty();
}
void LibraryController::RunLibraryPreloadFrame(const FrameInput &input) {
  if (!preload_) {
    app_.ShowSettingsView(false);
    return;
  }
  if (app_.ShouldAbortWork())
    return;
  auto &s = *preload_;
  if (s.wait_release) {
    if (!input.keys_held)
      s.wait_release = false;
    return;
  }
  const auto touch = input.MapTouch(app_.portrait_orientation);
  const bool pressed = (input.keys_down & (app_.key.b | app_.key.a)) ||
                       ((input.keys_down & KEY_TOUCH) &&
                        s.action.EnclosesPoint(touch.x, touch.y));
  if (pressed) {
    if (s.finished) {
      CloseLibraryPreload();
      return;
    }
    s.engine.Cancel();
    s.book.reset();
    s.start_pending = false;
    s.finished = true;
  } else if (s.start_pending) {
    s.start_pending = false;
    if (!cover_cache::EnableFullRetention()) {
      s.message = "Cannot save cover retention on SD";
      s.finished = true;
      DBG_LOGF(&app_, "LIBPRELOAD retention write failed");
    } else {
      s.finished =
          !s.engine.Start(PreloadRoot, this, PreloadMetadata, PreloadCover);
      if (s.finished)
        s.message = "Unable to start preload";
      DBG_LOGF(
          &app_, "LIBPRELOAD start root_entries=%u",
          (unsigned)(root_cached_ ? root_books_.size() : app_.books.size()));
    }
  } else if (!s.finished) {
    s.engine.Pump(16, PreloadClock, 4);
    if (s.engine.GetStatus() != library_preload::Status::Running) {
      s.finished = true;
      s.book.reset();
    }
  } else
    return;
  if (s.finished) {
#ifdef DSLIBRIS_DEBUG
    const auto &m = s.engine.MetadataCounts();
    const auto &c = s.engine.CoverCounts();
    DBG_LOGF(&app_,
             "LIBPRELOAD done status=%d books=%u metadata=%u/%u/%u/%u "
             "covers=%u/%u/%u/%u/%u folders_failed=%u",
             (int)s.engine.GetStatus(), (unsigned)s.engine.Examined(),
             (unsigned)m.cached, (unsigned)m.saved, (unsigned)m.unsupported,
             (unsigned)m.errors, (unsigned)c.cached, (unsigned)c.saved,
             (unsigned)c.absent, (unsigned)c.unsupported, (unsigned)c.errors,
             (unsigned)s.engine.DirectoryErrors());
#endif
  }
  DrawLibraryPreload();
}
void App::OpenLibraryPreload() {
  library_controller_->OpenLibraryPreload();
}
void App::RunLibraryPreloadFrame(const FrameInput &input) {
  library_controller_->RunLibraryPreloadFrame(input);
}

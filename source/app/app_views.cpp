/*
    3dslibris - app_views.cpp
    Extracted from app.cpp. Holds the Show*View entry points that switch
    AppMode and prepare the relevant submenu / screen state — font picker,
    library, settings, bookmarks, chapters/index, current book view — plus
    ReturnFromPrefs and the related layout-revision helpers.

    No behavior change — pure code motion.
*/

#include "app/app.h"

#include <3ds.h>

#include "book/book.h"
#include "book/book_renderer.h"
#include "menus/bookmark_menu.h"
#include "menus/chapter_menu.h"
#include "app/settings_controller.h"
#include "settings/font.h"
#include "shared/app_flow_utils.h"
#include "shared/debug_log.h"
#include "ui/text.h"
#include "ui/button.h"
#include "ui/screen_layout_constants.h"

namespace
{
  static const u64 kBrowserReturnWarmupCooldownMs = 1200;
} // namespace

void App::ShowFontView(AppMode app_font_mode)
{
  nav_.mode = AppMode::PrefsFont;
  ts->SetScreen(ts->screenright);
  ts->MarkScreenDirty(ts->screenright);
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this,
           "FONT show mode=%d screen=%p right=%p left=%p ts_dirty=%d req=%d",
           (int)nav_.mode, (void *)ts->GetScreen(), (void *)ts->screenright,
           (void *)ts->screenleft, ts->HasDirtyScreens() ? 1 : 0,
           (int)app_font_mode);
#endif
  fontmenu->Open(app_font_mode);
}

void App::ShowLibraryView()
{
  // Reset shared bottom buttons immediately; prefs view reuses/moves them.
  buttonprev.Move(screen_layout::kFooterLeftX, screen_layout::kFooterY);
  buttonprev.Resize(screen_layout::kFooterNavW, screen_layout::kFooterButtonH);
  buttonprev.Label("prev");
  buttonprev.SetIcon(UI_BUTTON_ICON_PREV);
  buttonback.Move(screen_layout::kFooterLeftX, screen_layout::kFooterY);
  buttonback.Resize(screen_layout::kFooterNavW, screen_layout::kFooterButtonH);
  buttonback.Label("back");
  buttonback.SetIcon(UI_BUTTON_ICON_BACK);
  buttonnext.Move(screen_layout::kFooterRightX, screen_layout::kFooterY);
  buttonnext.Resize(screen_layout::kFooterNavW, screen_layout::kFooterButtonH);
  buttonnext.Label("next");
  buttonnext.SetIcon(UI_BUTTON_ICON_NEXT);
  buttonprefs.Move(screen_layout::kFooterMidX, screen_layout::kFooterY);
  buttonprefs.Resize(screen_layout::kFooterMidW, screen_layout::kFooterButtonH);
  buttonprefs.Label("settings");
  buttonprefs.SetIcon(UI_BUTTON_ICON_GEAR);

  Book *bookcurrent_ = GetCurrentBook();
  if (bookcurrent_) {
    bookcurrent_->FlushPendingCacheSaves();
    // Cancel any in-progress PDF/CBZ strip render so the worker thread is
    // idle before browser warmup jobs can start touching MuPDF lock state.
    book_renderer::CancelFixedLayoutDeferredWork(bookcurrent_);
  }

  ResetBrowserMarquee();
  ResetPageRepeat();
  nav_.mode = AppMode::Browser;
  ts->SetScreen(ts->screenright);
  ts->MarkAllScreensDirty();
  nav_.browser.wait_input_release = true;
  nav_.browser.last_interaction_ms =
      osGetTime() + kBrowserReturnWarmupCooldownMs;
  nav_.browser.view_dirty = true;
  skip_next_browser_present_ = true;
  nav_.prefs.layout_notice_pending = false;
}

void App::ShowSettingsView(bool from_book)
{
  settings_controller_->ShowSettingsView(from_book);
}

void App::ReturnFromPrefs()
{
  if (IsBookSettingsContext() && GetCurrentBook()) {
    Book *book = GetCurrentBook();
    if (BookNeedsRelayout(book)) {
      SetSelectedBook(book);
      OpenBook();
    } else {
      ShowCurrentBookView();
      book_renderer::DrawCurrentView(book, ts.get());
      RequestStatusRedraw();
    }
  } else {
    ShowLibraryView();
  }
}

void App::MarkBookLayoutDirty()
{
  // Bump the global layout generation so already-paginated books are reopened
  // before they are reused.
  reader_state_.layout_revision++;
  if (reader_state_.layout_revision == 0)
    reader_state_.layout_revision = 1;
  nav_.prefs.view_dirty = true;
  if (nav_.prefs.from_book && reader_state_.bookcurrent && reader_state_.bookcurrent->GetPageCount() > 0)
    nav_.prefs.layout_notice_pending = true;
}

bool App::BookNeedsRelayout(Book *book) const
{
  if (!book || !book->UsesTextLayoutSettings())
    return false;
  return book && app_flow_utils::NeedsBookRelayout(
                     book->GetPageCount(), book->GetLayoutRevision(),
                     reader_state_.layout_revision, book->NeedsMobiRenderRefresh());
}

void App::ShowBookmarksView()
{
  nav_.mode = AppMode::Bookmarks;
  ts->SetScreen(ts->screenright);
  bookmarkmenu->Init();
}

void App::ShowChaptersView()
{
  DBG_LOG(this, "INDEX show begin");
  Book *book = reader_state_.bookcurrent;
  format_t format = FORMAT_UNDEF;
  bool toc_quality_known = false;
  if (book)
  {
    format = book->format;
    toc_quality_known = book->GetTocQuality() != TOC_QUALITY_UNKNOWN;
    DBG_LOGF(this,
             "INDEX request mode=%d book=%p fmt=%d chapters=%u tocq=%d tried=%d",
             (int)nav_.mode, (void *)book, (int)format,
             (unsigned)book->GetChapters().size(), (int)book->GetTocQuality(),
             book->tocResolveTried ? 1 : 0);
  }
  else
  {
    DBG_LOGF(this, "INDEX request mode=%d book=null", (int)nav_.mode);
  }
  app_flow_utils::ChaptersViewDecision decision =
      app_flow_utils::DecideChaptersView(
          book != nullptr, format, toc_quality_known,
          book ? book->tocResolveTried : false,
          book ? book->GetChapters().size() : 0);
  DBG_LOGF(this, "INDEX decision open=%d queue=%d reason=%d",
           decision.open_chapters ? 1 : 0, decision.queue_toc_resolve ? 1 : 0,
           (int)decision.reason);
  // Opening index must stay responsive. If chapters already exist, skip
  // deferred TOC resolve here and use the available chapter list.
  if (decision.queue_toc_resolve && book &&
      book->GetChapters().empty())
    QueueTocResolve(book);
  if (!decision.open_chapters)
  {
    if (decision.reason == app_flow_utils::ChaptersViewReason::NoCurrentBook)
    {
      PrintStatus("Index unavailable: no selected book");
    }
    else
    {
      PrintStatus("Index unavailable: no chapters");
    }
    ShowSettingsView(true);
    return;
  }
  if (!book || book->GetChapters().empty())
  {
    PrintStatus("Index unavailable: no chapters");
    ShowSettingsView(true);
    return;
  }
  nav_.mode = AppMode::Chapters;
  ts->SetScreen(ts->screenright);
  ts->MarkScreenDirty(ts->screenright);
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(this, "INDEX show screen=%p right=%p left=%p ts_dirty=%d",
           (void *)ts->GetScreen(), (void *)ts->screenright,
           (void *)ts->screenleft, ts->HasDirtyScreens() ? 1 : 0);
#endif
  DBG_LOG(this, "INDEX show init menu begin");
  chaptermenu->Init();
  chaptermenu->SelectChapterForPage((u16)book->GetPosition());
  chaptermenu->DisableInitialReleaseWait();
  DBG_LOG(this, "INDEX show init menu end");
  DBG_LOGF(this,
           "INDEX open chapters=%u page_count=%u reader_page=%u menu_page=%u",
           (unsigned)book->GetChapters().size(), (unsigned)book->GetPageCount(),
           (unsigned)book->GetPosition(), (unsigned)chaptermenu->GetCurrentPage());
}

void App::ShowCurrentBookView()
{
  if (!reader_state_.bookcurrent)
    return;
  nav_.mode = AppMode::Book;
  ts->SetScreen(ts->screenright);
  ts->MarkAllScreensDirty();
  RequestStatusRedraw();
}

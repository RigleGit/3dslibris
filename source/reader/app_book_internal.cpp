/*
    3dslibris - app_book_internal.cpp
    Shared implementation of helpers used by both app_book.cpp and
    app_book_open.cpp. Previously these lived in app_book.cpp's
    anonymous namespace; moving them here lets OpenBook live in its
    own translation unit without duplicating ~250 lines of helper code.

    No behavior change — bodies are byte-identical to the originals.
*/

#include "reader/app_book_internal.h"

#include "app/app.h"

#include <stdio.h>
#include <string>
#include <vector>

#include "book/book.h"
#include "reader/book_page_nav.h"
#include "reader/book_switch_utils.h"
#include "shared/debug_log.h"
#include "shared/string_utils.h"
#include "settings/prefs.h"
#include "ui/text.h"
#include "book/book_parser.h"
#include "formats/common/book_error.h"

namespace reader_internal {

const char *SafeBookName(Book *book) {
  if (!book)
    return "(null)";
  if (book->GetFileName() && *book->GetFileName())
    return book->GetFileName();
  if (book->GetTitle() && *book->GetTitle())
    return book->GetTitle();
  return "(untitled)";
}

std::list<int> CopyBookmarksAsInts(const std::list<u16> &bookmarks) {
  std::list<int> out;
  for (u16 bookmark : bookmarks)
    out.push_back((int)bookmark);
  return out;
}

void ApplyRemappedBookmarks(Book *book, const std::list<int> &bookmarks) {
  if (!book)
    return;
  std::list<u16> &dst = book->GetBookmarks();
  dst.clear();
  for (int bookmark : bookmarks)
    dst.push_back((u16)bookmark);
}

std::string EllipsizeToWidth(Text *ts, const std::string &text, int max_width,
                             u8 style) {
  if (!ts)
    return text;
  if ((int)ts->GetStringWidth(text.c_str(), style) <= max_width)
    return text;

  const char *ellipsis = "...";
  if ((int)ts->GetStringWidth(ellipsis, style) > max_width)
    return std::string();

  for (size_t len = text.size(); len > 0; --len) {
    std::string candidate = text.substr(0, len);
    candidate += ellipsis;
    if ((int)ts->GetStringWidth(candidate.c_str(), style) <= max_width)
      return candidate;
  }
  return std::string(ellipsis);
}

std::vector<std::string> BuildOpeningTitleLines(Text *ts, const char *name,
                                                int max_width, int max_lines,
                                                u8 style) {
  std::vector<std::string> lines;
  if (!ts || !name || !*name || max_lines <= 0)
    return lines;

  std::vector<std::string> tokens;
  std::string current_token;
  for (const char *p = name; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!current_token.empty()) {
        tokens.push_back(current_token);
        current_token.clear();
      }
    } else {
      current_token.push_back(*p);
    }
  }
  if (!current_token.empty())
    tokens.push_back(current_token);
  if (tokens.empty())
    return lines;

  std::string line;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string candidate =
        line.empty() ? tokens[i] : (line + " " + tokens[i]);
    if ((int)ts->GetStringWidth(candidate.c_str(), style) <= max_width) {
      line = candidate;
      continue;
    }

    if (line.empty()) {
      lines.push_back(EllipsizeToWidth(ts, tokens[i], max_width, style));
    } else {
      lines.push_back(line);
      line = tokens[i];
    }

    if ((int)lines.size() >= max_lines - 1 && i + 1 < tokens.size()) {
      std::string rest = line;
      for (size_t j = i + 1; j < tokens.size(); ++j) {
        if (!rest.empty())
          rest.push_back(' ');
        rest += tokens[j];
      }
      lines.push_back(EllipsizeToWidth(ts, Trim(rest), max_width, style));
      return lines;
    }
  }

  if (!line.empty() && (int)lines.size() < max_lines)
    lines.push_back(line);
  if ((int)lines.size() > max_lines)
    lines.resize((size_t)max_lines);
  return lines;
}

void ResetBookRenderState(App *app, bool clear_glyph_cache, const char *reason) {
  if (!app || !app->ts.get())
    return;
  if (clear_glyph_cache)
    app->ts->ClearCache();
  app->ts->MarkAllScreensDirty();
  app->RequestStatusRedraw();
  if (reason)
    DBG_LOG(app, reason);
}

bool ReuseParsedBook(App *app) {
  Book *selected = app ? app->GetSelectedBook() : NULL;
  if (!selected)
    return false;
  if (selected->IsCbz()) {
    selected->ResetCbzFailureState();
    selected->ResetCbzTransientViewState(true);
    DBG_LOG(app, "BOOK reopen: state reset complete");
  }
  app->SetCurrentBook(selected);
  if (app->GetMode() == AppMode::Browser) {
    if (app->orientation) {
      // lcdSwap(); // Not used on 3DS, keep for parity with original flow.
    }
    app->ShowCurrentBookView();
  }
  ResetBookRenderState(app, true,
                       "OpenBook: reset text renderer state (reuse)");
  Book *current = app->GetCurrentBook();
  if (current->GetPosition() >= current->GetPageCount())
    current->SetPosition(0);
  book_nav::DrawPage(current, app->ts.get());
  return true;
}

OpenBookRelayoutState CaptureRelayoutState(Book *book, bool needs_relayout) {
  OpenBookRelayoutState state = {needs_relayout, 0, 0, std::list<int>()};
  if (!book || !needs_relayout)
    return state;

  state.old_page_count = book->GetPageCount();
  state.old_position = book->GetPosition();
  state.old_bookmarks = CopyBookmarksAsInts(book->GetBookmarks());
  book->Close();
  return state;
}

void DrawOpeningSplashImpl(App *app, unsigned spine_done, unsigned spine_total,
                           const char *label) {
  Book *selected = app ? app->GetSelectedBook() : NULL;
  if (!app || !app->ts || !selected)
    return;

  int savedStyle = app->ts->GetStyle();
  int savedColorMode = app->ts->GetColorMode();
  u16 *savedScreen = app->ts->GetScreen();

  app->ts->SetStyle(TEXT_STYLE_BROWSER);
  app->ts->PrintSplash(app->ts->screenleft);

  app->ts->SetScreen(app->ts->screenright);
  app->ts->ClearScreen();
  app->DrawBottomGradientBackground();
  app->ts->SetPen(12, 28);
  app->ts->PrintString(label ? label : "opening book ...");

  const char *name = selected->GetFileName();
  if (!name || !*name)
    name = selected->GetTitle();
  if (name && *name) {
    std::vector<std::string> lines = BuildOpeningTitleLines(
        app->ts.get(), name, kOpeningTitleMaxWidth, kOpeningTitleMaxLines,
        TEXT_STYLE_BROWSER);
    for (size_t i = 0; i < lines.size(); ++i) {
      app->ts->SetPen(12, (u16)(50 + (int)i * kOpeningTitleLineHeight));
      app->ts->PrintString(lines[i].c_str());
    }
  }

  if (spine_total > 0) {
    unsigned pct = spine_done * 100u / spine_total;
    char progress[32];
    snprintf(progress, sizeof(progress), "%u / %u  (%u%%)", spine_done,
             spine_total, pct);
    app->ts->SetPen(12, 106);
    app->ts->PrintString(progress);
  }

  app->ts->SetStyle(savedStyle);
  app->ts->SetColorMode(savedColorMode);
  app->ts->SetScreen(savedScreen);

  if (app->ShouldAbortWork())
    return;
  if (app->ts->BlitToFramebuffer()) {
    gfxFlushBuffers();
    gfxSwapBuffers();
  }
}

void DrawOpeningSplash(App *app) { DrawOpeningSplashImpl(app, 0, 0); }

void MaybeDrawOpeningSplashProgress(App *app) {
  if (!app || !app->IsOpeningPending() || !app->GetOpeningBook())
    return;

  const unsigned int seq = app->GetOpeningProgressSeq();
  if (seq == 0 || seq == app->GetOpeningDrawnProgressSeq())
    return;

  DrawOpeningSplashImpl(app, app->GetOpeningSpineDone(),
                        app->GetOpeningSpineTotal());
  app->SetOpeningDrawnProgressSeq(seq);
}

void ResetOpeningState(App *app) {
  if (!app)
    return;
  app->SetOpeningPending(false);
  app->SetOpeningBook(NULL);
  app->SetOpeningSessionId(0);
  app->SetOpeningNeedsRelayout(false);
  app->SetOpeningOldPageCount(0);
  app->SetOpeningOldPosition(0);
  app->SetOpeningSpineProgress(0, 0);
  app->SetOpeningDrawnProgressSeq(0);
  app->MutableOpeningOldBookmarks().clear();
  app->SetOpeningStartedAtMs(0);
}

void DetachCurrentBookForSwitch(App *app, Book *next_book,
                                unsigned int next_session_id,
                                const char *reason) {
  if (!app)
    return;
  Book *current = app->GetCurrentBook();
  if (!ShouldCloseCurrentBookForSwitch(current, next_book))
    return;

  DBG_LOGF(app,
           "BOOK switch: close current session=%u current=%s next_session=%u next=%s reason=%s",
           app->GetCurrentBookSessionId(), SafeBookName(current),
           next_session_id, SafeBookName(next_book), reason ? reason : "");
  app->SetCurrentBook(NULL);
  app->SetCurrentBookSessionId(0);
  app->SetPdfDeferredReadyAtMs(0);
  current->Close();
  DBG_LOGF(app, "BOOK switch: current closed current=%s", SafeBookName(current));
}

u8 OpenSelectedBook(App *app, unsigned int session_id) {
  Book *selected = app ? app->GetSelectedBook() : NULL;
  if (!selected)
    return 254;

  DetachCurrentBookForSwitch(app, selected, session_id, "sync-open");
  selected->SetOpenSessionId(session_id);
  DBG_LOG(app, "BOOK open path: synchronous");
  DBG_LOGF(app, "BOOK open begin session=%u book=%s", session_id,
           SafeBookName(selected));
  if (int err = book_parser::Open(selected)) {
    DBG_LOGF(app, "BOOK open fail session=%u rc=%d book=%s", session_id, err,
             SafeBookName(selected));
    if (const char *desc = DescribeBookOpenError(err)) {
      app->PrintStatus(desc);
    } else {
      char msg[64];
      snprintf(msg, sizeof(msg), "error (%d)", err);
      app->PrintStatus(msg);
    }
    return (u8)err;
  }
  DBG_LOGF(app, "BOOK open success session=%u book=%s", session_id,
           SafeBookName(selected));
  return 0;
}

void CloseFailedOpenBook(App *app, Book *book, unsigned int session_id,
                         const char *reason) {
  if (!book)
    return;
  DBG_LOGF(app, "BOOK open cleanup: session=%u reason=%s pages=%d book=%s",
           session_id, reason ? reason : "", book->GetPageCount(),
           SafeBookName(book));
  book->Close();
}

void EnsureBookMode(App *app, const char *log_message) {
  if (!app || app->GetMode() != AppMode::Browser || app->ShouldAbortWork())
    return;
  if (app->orientation) {
    // lcdSwap(); // Not used on 3DS, keep for parity with original flow.
  }
  app->ShowCurrentBookView();
  if (log_message)
    DBG_LOG(app, log_message);
}

} // namespace reader_internal

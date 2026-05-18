// Shared helpers between source/reader/app_book.cpp and
// source/reader/app_book_open.cpp. Internal to the reader controller — do
// not include from outside source/reader/.

#pragma once

#include <3ds.h>

#include <list>
#include <string>
#include <vector>

class App;
class Book;
class Text;

namespace reader_internal {

constexpr int kOpeningTitleMaxWidth = 216;
constexpr int kOpeningTitleMaxLines = 3;
constexpr int kOpeningTitleLineHeight = 16;

const char *SafeBookName(Book *book);
std::list<int> CopyBookmarksAsInts(const std::list<u16> &bookmarks);
void ApplyRemappedBookmarks(Book *book, const std::list<int> &bookmarks);

struct OpenBookRelayoutState {
  bool needs_relayout;
  int old_page_count;
  int old_position;
  std::list<int> old_bookmarks;
};

std::string EllipsizeToWidth(Text *ts, const std::string &text, int max_width,
                             u8 style);
std::vector<std::string> BuildOpeningTitleLines(Text *ts, const char *name,
                                                int max_width, int max_lines,
                                                u8 style);

void ResetBookRenderState(App *app, bool clear_glyph_cache, const char *reason);
bool ReuseParsedBook(App *app);
OpenBookRelayoutState CaptureRelayoutState(Book *book, bool needs_relayout);

void DrawOpeningSplashImpl(App *app, unsigned spine_done, unsigned spine_total,
                           const char *label = "opening book ...");
void DrawOpeningSplash(App *app);
void MaybeDrawOpeningSplashProgress(App *app);

void ResetOpeningState(App *app);
void DetachCurrentBookForSwitch(App *app, Book *next_book,
                                unsigned int next_session_id,
                                const char *reason);
u8 OpenSelectedBook(App *app, unsigned int session_id);
void CloseFailedOpenBook(App *app, Book *book, unsigned int session_id,
                         const char *reason);
void EnsureBookMode(App *app, const char *log_message);

} // namespace reader_internal

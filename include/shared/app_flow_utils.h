#pragma once

#include <list>
#include <string>

namespace app_flow_utils {

enum class BookFileFormat {
  Unsupported = 0,
  XhtmlLike = 1,
  Epub = 2,
  MuPdf = 3,
};

enum class BookmarkJumpDirection {
  Next = 0,
  Previous = 1,
};

struct BookmarkJumpResult {
  bool found;
  unsigned short page;
};

enum class ChaptersViewReason {
  OpenChapters = 0,
  NoCurrentBook = 1,
  NoChapters = 2,
};

struct ChaptersViewDecision {
  bool open_chapters;
  bool queue_toc_resolve;
  ChaptersViewReason reason;
};

struct StatusSnapshotInput {
  const void *current_book;
  int page_num;
  int live_page_count;
  bool deferred_pagination;
  const void *locked_book;
  int locked_pagecount;
};

struct StatusSnapshot {
  bool has_progress;
  int draw_page_count;
  float percent_value;
  int percent_tenths;
  const void *next_locked_book;
  int next_locked_pagecount;
};

BookFileFormat DetectBookFormat(const char *filename);
bool ShouldIndexBookFilename(const char *filename);
bool SupportsMetadataIndexing(BookFileFormat format);
std::string SdmcToArchiveRelPath(const std::string &path);
bool NeedsBookRelayout(int page_count, unsigned int book_layout_revision,
                       unsigned int app_layout_revision,
                       bool needs_mobi_refresh);
BookmarkJumpResult FindBookmarkJumpTarget(
    const std::list<unsigned short> &bookmarks, unsigned short current_page,
    BookmarkJumpDirection direction);
ChaptersViewDecision DecideChaptersView(bool has_current_book,
                                        BookFileFormat format,
                                        bool toc_quality_known,
                                        bool toc_resolve_tried,
                                        size_t chapter_count);
StatusSnapshot ComputeStatusSnapshot(const StatusSnapshotInput &input);

} // namespace app_flow_utils

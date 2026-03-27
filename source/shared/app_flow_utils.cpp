#include "shared/app_flow_utils.h"

#include <cstring>

namespace app_flow_utils {
namespace {

bool EndsWithNoCase(const char *value, const char *suffix) {
  if (!value || !suffix)
    return false;
  size_t value_len = std::strlen(value);
  size_t suffix_len = std::strlen(suffix);
  if (value_len < suffix_len)
    return false;
  return strcasecmp(value + value_len - suffix_len, suffix) == 0;
}

} // namespace

BookFileFormat DetectBookFormat(const char *filename) {
  if (!filename)
    return BookFileFormat::Unsupported;
  if (EndsWithNoCase(filename, ".epub"))
    return BookFileFormat::Epub;
  if (EndsWithNoCase(filename, ".pdf") || EndsWithNoCase(filename, ".cbz"))
    return BookFileFormat::MuPdf;
  if (EndsWithNoCase(filename, ".fb2") || EndsWithNoCase(filename, ".txt") ||
      EndsWithNoCase(filename, ".rtf") || EndsWithNoCase(filename, ".odt") ||
      EndsWithNoCase(filename, ".mobi")) {
    return BookFileFormat::XhtmlLike;
  }
  return BookFileFormat::Unsupported;
}

bool ShouldIndexBookFilename(const char *filename) {
  if (!filename || !*filename)
    return false;
  if (filename[0] == '.')
    return false;
  return DetectBookFormat(filename) != BookFileFormat::Unsupported;
}

bool SupportsMetadataIndexing(BookFileFormat format) {
  return format == BookFileFormat::Epub || format == BookFileFormat::MuPdf;
}

std::string SdmcToArchiveRelPath(const std::string &path) {
  static const char kPrefix[] = "sdmc:";
  if (path.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0) {
    std::string rel = path.substr(sizeof(kPrefix) - 1);
    if (rel.empty())
      return "/";
    if (rel[0] != '/')
      rel.insert(rel.begin(), '/');
    return rel;
  }
  if (!path.empty() && path[0] != '/')
    return std::string("/") + path;
  return path;
}

bool NeedsBookRelayout(int page_count, unsigned int book_layout_revision,
                       unsigned int app_layout_revision,
                       bool needs_mobi_refresh) {
  if (page_count <= 0)
    return false;
  return book_layout_revision != app_layout_revision || needs_mobi_refresh;
}

BookmarkJumpResult FindBookmarkJumpTarget(
    const std::list<unsigned short> &bookmarks, unsigned short current_page,
    BookmarkJumpDirection direction) {
  BookmarkJumpResult result = {false, 0};
  if (bookmarks.empty())
    return result;

  if (direction == BookmarkJumpDirection::Previous) {
    for (std::list<unsigned short>::const_reverse_iterator it =
             bookmarks.rbegin();
         it != bookmarks.rend(); ++it) {
      if (*it < current_page) {
        result.found = true;
        result.page = *it;
        return result;
      }
    }
    result.found = true;
    result.page = *bookmarks.rbegin();
    return result;
  }

  for (std::list<unsigned short>::const_iterator it = bookmarks.begin();
       it != bookmarks.end(); ++it) {
    if (*it > current_page) {
      result.found = true;
      result.page = *it;
      return result;
    }
  }

  result.found = true;
  result.page = *bookmarks.begin();
  return result;
}

ChaptersViewDecision DecideChaptersView(bool has_current_book,
                                        BookFileFormat format,
                                        bool toc_quality_known,
                                        bool toc_resolve_tried,
                                        size_t chapter_count) {
  if (!has_current_book) {
    return {false, false, ChaptersViewReason::NoCurrentBook};
  }
  if (chapter_count > 0) {
    const bool queue_toc_resolve =
        format == BookFileFormat::Epub && !toc_quality_known &&
        !toc_resolve_tried;
    return {true, queue_toc_resolve, ChaptersViewReason::OpenChapters};
  }
  return {false, format == BookFileFormat::Epub && !toc_quality_known &&
                      !toc_resolve_tried,
          ChaptersViewReason::NoChapters};
}

StatusSnapshot ComputeStatusSnapshot(const StatusSnapshotInput &input) {
  StatusSnapshot snapshot = {false, 0, 0.0f, -1, nullptr, 0};
  if (!input.current_book || input.live_page_count <= 0) {
    return snapshot;
  }

  snapshot.next_locked_book = input.current_book;

  if (input.deferred_pagination) {
    int locked_pagecount =
        (input.locked_book == input.current_book) ? input.locked_pagecount : 0;
    if (locked_pagecount <= 0)
      locked_pagecount = input.live_page_count;
    if (locked_pagecount < input.page_num + 1)
      locked_pagecount = input.page_num + 1;
    snapshot.next_locked_pagecount = locked_pagecount;
    return snapshot;
  }

  int draw_page_count = input.live_page_count;
  if (draw_page_count < 1)
    draw_page_count = 1;

  snapshot.has_progress = true;
  snapshot.draw_page_count = draw_page_count;
  snapshot.next_locked_pagecount = 0;
  snapshot.percent_value =
      draw_page_count > 1
          ? ((float)input.page_num / (float)(draw_page_count - 1)) * 100.0f
          : 100.0f;
  snapshot.percent_tenths = (int)(snapshot.percent_value * 10.0f + 0.5f);
  return snapshot;
}

} // namespace app_flow_utils

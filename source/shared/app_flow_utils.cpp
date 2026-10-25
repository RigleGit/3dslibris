#include "shared/app_flow_utils.h"

#include "shared/debug_runtime_mode.h"
#include "shared/string_utils.h"

namespace app_flow_utils {

format_t DetectBookFormat(const char *filename) {
  if (!filename)
    return FORMAT_UNDEF;
  if (HasExtCI(filename, ".epub"))
    return FORMAT_EPUB;
  if (HasExtCI(filename, ".pdf") || HasExtCI(filename, ".xps") ||
      HasExtCI(filename, ".oxps"))
    return FORMAT_PDF;
  if (HasExtCI(filename, ".cbz"))
    return CbzSupportEnabled() ? FORMAT_CBZ
                               : FORMAT_UNDEF;
  if (HasExtCI(filename, ".fb2") || HasExtCI(filename, ".txt") ||
      HasExtCI(filename, ".md") || HasExtCI(filename, ".markdown") ||
      HasExtCI(filename, ".rtf") || HasExtCI(filename, ".odt") ||
      HasExtCI(filename, ".mobi")) {
    return FORMAT_XHTML;
  }
  return FORMAT_UNDEF;
}

bool CbzSupportEnabled() { return true; }

MuPdfDocumentKind DetectMuPdfDocumentKind(const char *filename) {
  if (!filename)
    return MuPdfDocumentKind::Unknown;
  if (HasExtCI(filename, ".pdf"))
    return MuPdfDocumentKind::Pdf;
  if (HasExtCI(filename, ".xps") || HasExtCI(filename, ".oxps"))
    return MuPdfDocumentKind::Xps;
  return MuPdfDocumentKind::Unknown;
}

const char *GetMuPdfDocumentLabel(MuPdfDocumentKind kind) {
  switch (kind) {
  case MuPdfDocumentKind::Pdf:
    return "PDF";
  case MuPdfDocumentKind::Xps:
    return "XPS";
  case MuPdfDocumentKind::Unknown:
  default:
    return "MuPDF";
  }
}

float GetMuPdfReadingBaseZoom(MuPdfDocumentKind kind) {
  switch (kind) {
  case MuPdfDocumentKind::Pdf:
  case MuPdfDocumentKind::Xps:
  case MuPdfDocumentKind::Unknown:
  default:
    return 1.5f;
  }
}

bool MuPdfWantsFinalQualityRender(MuPdfDocumentKind kind) {
  (void)kind;
  return !debug_runtime::ForceSynchronousMuPdfRender();
}

bool MuPdfShouldPrefetchAdjacent(MuPdfDocumentKind kind) {
  (void)kind;
  return false;
}

bool ShouldIndexBookFilename(const char *filename) {
  if (!filename || !*filename)
    return false;
  if (filename[0] == '.')
    return false;
  return DetectBookFormat(filename) != FORMAT_UNDEF;
}

bool SupportsMetadataIndexing(format_t format) {
  return format == FORMAT_EPUB || format == FORMAT_PDF;
}

bool ShouldReopenBookOnStartup(bool reopen_enabled, bool has_current_book) {
  return reopen_enabled && has_current_book;
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
                                        format_t format,
                                        bool toc_quality_known,
                                        bool toc_resolve_tried,
                                        size_t chapter_count) {
  if (!has_current_book) {
    return {false, false, ChaptersViewReason::NoCurrentBook};
  }
  if (chapter_count > 0) {
    const bool queue_toc_resolve =
        format == FORMAT_EPUB && !toc_quality_known &&
        !toc_resolve_tried;
    return {true, queue_toc_resolve, ChaptersViewReason::OpenChapters};
  }
  return {false, format == FORMAT_EPUB && !toc_quality_known &&
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

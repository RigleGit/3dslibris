#include "shared/app_flow_utils.h"

#include <cstdio>
#include <cstdlib>
#include <list>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

template <typename T>
void ExpectEq(const char *label, const T &actual, const T &expected) {
  if (!(actual == expected)) {
    Fail(std::string(label) + ": expected equality");
  }
}

void TestDetectBookFormat() {
  using app_flow_utils::BookFileFormat;

  ExpectEq("epub", app_flow_utils::DetectBookFormat("book.epub"),
           BookFileFormat::Epub);
  ExpectEq("epub uppercase", app_flow_utils::DetectBookFormat("BOOK.EPUB"),
           BookFileFormat::Epub);
  ExpectEq("txt", app_flow_utils::DetectBookFormat("book.txt"),
           BookFileFormat::XhtmlLike);
  ExpectEq("mobi", app_flow_utils::DetectBookFormat("book.mobi"),
           BookFileFormat::XhtmlLike);
  ExpectEq("pdf", app_flow_utils::DetectBookFormat("book.pdf"),
           BookFileFormat::Pdf);
}

void TestShouldIndexBookFilename() {
  ExpectTrue("regular epub indexed",
             app_flow_utils::ShouldIndexBookFilename("book.epub"));
  ExpectFalse("dotfile skipped",
              app_flow_utils::ShouldIndexBookFilename(".hidden.epub"));
  ExpectTrue("pdf indexed",
             app_flow_utils::ShouldIndexBookFilename("notes.pdf"));
  ExpectFalse("empty skipped", app_flow_utils::ShouldIndexBookFilename(""));
}

void TestSupportsMetadataIndexing() {
  using app_flow_utils::BookFileFormat;

  ExpectTrue("epub metadata indexing",
             app_flow_utils::SupportsMetadataIndexing(BookFileFormat::Epub));
  ExpectTrue("pdf metadata indexing",
             app_flow_utils::SupportsMetadataIndexing(BookFileFormat::Pdf));
  ExpectFalse("xhtml-like no metadata indexing",
              app_flow_utils::SupportsMetadataIndexing(
                  BookFileFormat::XhtmlLike));
  ExpectFalse("unsupported no metadata indexing",
              app_flow_utils::SupportsMetadataIndexing(
                  BookFileFormat::Unsupported));
}

void TestSdmcToArchiveRelPath() {
  ExpectEq("sdmc prefix stripped",
           app_flow_utils::SdmcToArchiveRelPath("sdmc:/3ds/3dslibris/book"),
           std::string("/3ds/3dslibris/book"));
  ExpectEq("empty suffix maps root",
           app_flow_utils::SdmcToArchiveRelPath("sdmc:"), std::string("/"));
  ExpectEq("plain relative path gets slash",
           app_flow_utils::SdmcToArchiveRelPath("books/sample.epub"),
           std::string("/books/sample.epub"));
}

void TestNeedsBookRelayout() {
  ExpectFalse("no pages no relayout",
              app_flow_utils::NeedsBookRelayout(0, 4u, 5u, false));
  ExpectTrue("layout revision mismatch",
             app_flow_utils::NeedsBookRelayout(12, 4u, 5u, false));
  ExpectTrue("mobi refresh forces relayout",
             app_flow_utils::NeedsBookRelayout(12, 5u, 5u, true));
  ExpectFalse("matching revision no refresh",
              app_flow_utils::NeedsBookRelayout(12, 5u, 5u, false));
}

void TestFindBookmarkJumpTarget() {
  using app_flow_utils::BookmarkJumpDirection;
  using app_flow_utils::BookmarkJumpResult;

  std::list<unsigned short> bookmarks;
  bookmarks.push_back(2);
  bookmarks.push_back(10);
  bookmarks.push_back(20);

  BookmarkJumpResult next = app_flow_utils::FindBookmarkJumpTarget(
      bookmarks, 5, BookmarkJumpDirection::Next);
  ExpectTrue("next found", next.found);
  ExpectEq("next page", next.page, (unsigned short)10);

  BookmarkJumpResult prev = app_flow_utils::FindBookmarkJumpTarget(
      bookmarks, 12, BookmarkJumpDirection::Previous);
  ExpectTrue("previous found", prev.found);
  ExpectEq("previous page", prev.page, (unsigned short)10);

  BookmarkJumpResult wrap_next = app_flow_utils::FindBookmarkJumpTarget(
      bookmarks, 25, BookmarkJumpDirection::Next);
  ExpectTrue("wrap next found", wrap_next.found);
  ExpectEq("wrap next page", wrap_next.page, (unsigned short)2);

  BookmarkJumpResult wrap_prev = app_flow_utils::FindBookmarkJumpTarget(
      bookmarks, 1, BookmarkJumpDirection::Previous);
  ExpectTrue("wrap previous found", wrap_prev.found);
  ExpectEq("wrap previous page", wrap_prev.page, (unsigned short)20);

  BookmarkJumpResult none = app_flow_utils::FindBookmarkJumpTarget(
      std::list<unsigned short>(), 5, BookmarkJumpDirection::Next);
  ExpectFalse("empty bookmarks no target", none.found);
}

void TestDecideChaptersView() {
  using app_flow_utils::BookFileFormat;
  using app_flow_utils::ChaptersViewReason;

  app_flow_utils::ChaptersViewDecision no_book =
      app_flow_utils::DecideChaptersView(false, BookFileFormat::Unsupported,
                                         false, false, 0);
  ExpectFalse("no book does not open chapters", no_book.open_chapters);
  ExpectFalse("no book does not queue toc", no_book.queue_toc_resolve);
  ExpectEq("no book reason", no_book.reason,
           ChaptersViewReason::NoCurrentBook);

  app_flow_utils::ChaptersViewDecision epub_needs_toc =
      app_flow_utils::DecideChaptersView(true, BookFileFormat::Epub, false,
                                         false, 0);
  ExpectFalse("epub without toc does not open chapters yet",
              epub_needs_toc.open_chapters);
  ExpectTrue("epub without toc queues resolve", epub_needs_toc.queue_toc_resolve);
  ExpectEq("epub without toc reason", epub_needs_toc.reason,
           ChaptersViewReason::NoChapters);

  app_flow_utils::ChaptersViewDecision epub_fallback_chapters =
      app_flow_utils::DecideChaptersView(true, BookFileFormat::Epub, false,
                                         false, 12);
  ExpectTrue("epub fallback chapters open", epub_fallback_chapters.open_chapters);
  ExpectTrue("epub fallback chapters queue resolve",
             epub_fallback_chapters.queue_toc_resolve);
  ExpectEq("epub fallback chapters reason", epub_fallback_chapters.reason,
           ChaptersViewReason::OpenChapters);

  app_flow_utils::ChaptersViewDecision epub_known_toc =
      app_flow_utils::DecideChaptersView(true, BookFileFormat::Epub, true,
                                         false, 12);
  ExpectTrue("epub known toc opens", epub_known_toc.open_chapters);
  ExpectFalse("epub known toc does not queue resolve",
              epub_known_toc.queue_toc_resolve);
  ExpectEq("epub known toc reason", epub_known_toc.reason,
           ChaptersViewReason::OpenChapters);

  app_flow_utils::ChaptersViewDecision empty_after_try =
      app_flow_utils::DecideChaptersView(true, BookFileFormat::Epub, false,
                                         true, 0);
  ExpectFalse("no chapters after toc attempt stays out",
              empty_after_try.open_chapters);
  ExpectFalse("no chapters after toc attempt does not queue again",
              empty_after_try.queue_toc_resolve);
  ExpectEq("no chapters after toc attempt reason", empty_after_try.reason,
           ChaptersViewReason::NoChapters);

  app_flow_utils::ChaptersViewDecision has_chapters =
      app_flow_utils::DecideChaptersView(true, BookFileFormat::XhtmlLike, false,
                                         true, 3);
  ExpectTrue("chapters open", has_chapters.open_chapters);
  ExpectFalse("chapters do not queue toc", has_chapters.queue_toc_resolve);
  ExpectEq("chapters reason", has_chapters.reason,
           ChaptersViewReason::OpenChapters);
}

void TestComputeStatusSnapshot() {
  using app_flow_utils::StatusSnapshot;
  using app_flow_utils::StatusSnapshotInput;

  StatusSnapshot normal = app_flow_utils::ComputeStatusSnapshot(
      StatusSnapshotInput{reinterpret_cast<const void *>(1), 2, 10, false,
                          nullptr, 0});
  ExpectTrue("normal has progress", normal.has_progress);
  ExpectEq("normal draw page count", normal.draw_page_count, 10);
  ExpectEq("normal percent tenths", normal.percent_tenths, 222);
  ExpectEq("normal lock book", normal.next_locked_book,
           reinterpret_cast<const void *>(1));
  ExpectEq("normal lock pagecount", normal.next_locked_pagecount, 0);

  StatusSnapshot deferred = app_flow_utils::ComputeStatusSnapshot(
      StatusSnapshotInput{reinterpret_cast<const void *>(2), 3, 5, true,
                          reinterpret_cast<const void *>(2), 0});
  ExpectFalse("deferred hides progress", deferred.has_progress);
  ExpectEq("deferred uses live count as initial lock",
           deferred.next_locked_pagecount, 5);
  ExpectEq("deferred keeps book lock", deferred.next_locked_book,
           reinterpret_cast<const void *>(2));

  StatusSnapshot deferred_grow = app_flow_utils::ComputeStatusSnapshot(
      StatusSnapshotInput{reinterpret_cast<const void *>(2), 7, 5, true,
                          reinterpret_cast<const void *>(2), 5});
  ExpectEq("deferred lock follows current page", deferred_grow.next_locked_pagecount,
           8);

  StatusSnapshot changed_book = app_flow_utils::ComputeStatusSnapshot(
      StatusSnapshotInput{reinterpret_cast<const void *>(3), 0, 4, true,
                          reinterpret_cast<const void *>(2), 9});
  ExpectEq("book change resets lock then seeds live count",
           changed_book.next_locked_pagecount, 4);
  ExpectEq("book change updates lock book", changed_book.next_locked_book,
           reinterpret_cast<const void *>(3));

  StatusSnapshot no_book = app_flow_utils::ComputeStatusSnapshot(
      StatusSnapshotInput{nullptr, 0, 0, false, reinterpret_cast<const void *>(4),
                          7});
  ExpectFalse("no book no progress", no_book.has_progress);
  ExpectEq("no book clears lock book", no_book.next_locked_book,
           static_cast<const void *>(nullptr));
  ExpectEq("no book clears lock pagecount", no_book.next_locked_pagecount, 0);
}

} // namespace

int main() {
  TestDetectBookFormat();
  TestShouldIndexBookFilename();
  TestSupportsMetadataIndexing();
  TestSdmcToArchiveRelPath();
  TestNeedsBookRelayout();
  TestFindBookmarkJumpTarget();
  TestDecideChaptersView();
  TestComputeStatusSnapshot();
  return 0;
}

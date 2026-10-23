#include "book/book.h"
#include "book/book_context.h"
#include "book/book_parser.h"
#include "book/book_renderer.h"
#include "book/book_parse_deps.h"
#include "book/page.h"
#include "formats/common/page_text_extract_utils.h"
#include "formats/common/book_error.h"
#include "formats/common/plain_parser.h"
#include "expat.h"
#include "formats/mobi/mobi_text_decode.h"
#include "formats/mobi/mobi_page_cache.h"
#include "formats/epub/epub_page_cache.h"
#include "minizip/unzip.h"
#include "shared/app_flow_utils.h"
#include "shared/open_cancel_poll.h"
#include "shared/status_reporter.h"
#include "shared/cover_decode_utils.h"
#include "shared/stb_image_gif_utils.h"
#include "shared/text_token_constants.h"
#include "ui/text.h"
#include "test_assert.h"

#include <algorithm>
#include <cassert>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <map>
#include <string>
#include <sys/stat.h>
#include <utime.h>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "tests/fixtures"
#endif

namespace {
bool reject_nothrow_allocations = false;
unsigned rejected_nothrow_allocations = 0;
unsigned cover_decode_calls = 0;
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  if (reject_nothrow_allocations) {
    ++rejected_nothrow_allocations;
    return nullptr;
  }
  return ::operator new(size);
}

namespace stb_image_gif_utils {
unsigned char *LoadFromMemoryForCoverTest(const unsigned char *, int, int *, int *, int *, int);
unsigned char *LoadFromMemory(const unsigned char *data, int size, int *width,
                              int *height, int *channels, int requested_channels) {
  ++cover_decode_calls;
  return LoadFromMemoryForCoverTest(data, size, width, height, channels,
                                    requested_channels);
}
}

namespace {

std::string BookText(Book *book) {
  std::string text;
  for (int p = 0; p < book->GetPageCount(); ++p) {
    const std::vector<std::string> lines =
        page_text_extract_utils::ExtractTextLinesFromPage(book->GetPage(p));
    for (size_t i = 0; i < lines.size(); ++i) {
      text += lines[i];
      text += ' ';
    }
  }
  return text;
}

void TestDispatchContentAndCloseBetweenFormats() {
  struct Case {
    const char *name;
    const char *content;
    const char *excluded_markup;
  };
  const Case cases[] = {
      {"basic.txt", "tiny TXT fixture", "tiny FB2 fixture"},
      {"basic.fb2", "tiny FB2 fixture", "<FictionBook"},
      {"basic.rtf", "tiny RTF fixture", "\\rtf1"},
      {"basic.md", "small Markdown fixture", "**small**"},
  };
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  book.SetFolderName(TEST_FIXTURES_DIR "/books");
  std::string previous_text;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    book.SetFileName(cases[i].name);
    book.format = FORMAT_UNDEF; // Dispatch must recognize the file extension.
    test::ExpectEq("format dispatch opens actual fixture", book_parser::Open(&book), 0);
    test::ExpectGt("format dispatch creates pages", book.GetPageCount(), 0);
    const std::string rendered = BookText(&book);
    test::ExpectStrContains(cases[i].name, rendered.c_str(), cases[i].content);
    test::ExpectTrue("format markup is consumed", rendered.find(cases[i].excluded_markup) == std::string::npos);
    if (i > 0)
      test::ExpectTrue("previous format content is gone", rendered.find(cases[i-1].content) == std::string::npos);
    if (i == 1) {
      const std::vector<ChapterEntry> &chapters = book.GetChapters();
      test::ExpectTrue("FB2 section creates navigation", !chapters.empty());
      test::ExpectStrEq("FB2 section title", chapters[0].title.c_str(), "Chapter 1");
      test::ExpectEq("FB2 basic section is top level", (int)chapters[0].level, 0);
    }
    if (i == 3) {
      test::ExpectStrContains("Markdown link label survives", rendered.c_str(), "link text");
      test::ExpectTrue("Markdown URL is not visible prose", rendered.find("https://example.com") == std::string::npos);
      test::ExpectStrContains("Markdown first heading survives", rendered.c_str(), "Markdown Chapter");
      test::ExpectStrContains("Markdown second heading survives", rendered.c_str(), "Second Section");
      test::ExpectTrue("Markdown heading markers consumed", rendered.find("# Markdown") == std::string::npos);
    }
    previous_text = rendered;
    book.Close();
    test::ExpectEq("close releases pages", book.GetPageCount(), 0);
    test::ExpectTrue("close releases chapters", book.GetChapters().empty());
    test::ExpectEqU("close releases inline links", book.GetInlineLinkHrefCount(), 0);
  }
  book.SetFileName("basic.md");
  test::ExpectEq("reopen final format", book_parser::Open(&book), 0);
  test::ExpectStrEq("reopen retains exactly the same content", BookText(&book).c_str(), previous_text.c_str());
  book.Close();

  book.SetFileName("missing-audit-fixture.txt");
  test::ExpectTrue("missing TXT returns error", book_parser::Open(&book) != 0);
  test::ExpectEq("failed open exposes no old pages", book.GetPageCount(), 0);
  book.Close();
  book.SetFileName("basic.txt");
  test::ExpectEq("open recovers after missing file", book_parser::Open(&book), 0);
  test::ExpectStrContains("recovery uses requested TXT", BookText(&book).c_str(), "tiny TXT fixture");
  book.Close();
}

void TestXmlAllocationFailureAndRecovery() {
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  book.SetFolderName(TEST_FIXTURES_DIR "/books");
  book.SetFileName("basic.fb2");
  const bool modes[] = {false, true};
  for (bool fulltext : modes) {
    rejected_nothrow_allocations = 0;
    reject_nothrow_allocations = true;
    const int rc = book_parser::Parse(&book, fulltext);
    reject_nothrow_allocations = false;
    test::ExpectEq("XML state allocation failure propagates", rc,
                   XML_ERROR_NO_MEMORY);
    test::ExpectEqU("XML handles the failed allocation once",
                    rejected_nothrow_allocations, 1);
    test::ExpectEq("XML failure creates no pages", book.GetPageCount(), 0);
    test::ExpectTrue("XML failure creates no chapters", book.GetChapters().empty());
    book.Close();
  }
  test::ExpectEq("XML retry succeeds", book_parser::Parse(&book, true), 0);
  test::ExpectStrContains("FB2 recovery preserves text", BookText(&book).c_str(),
                         "tiny FB2 fixture");
  test::ExpectEq("FB2 recovery retains its chapter", (int)book.GetChapters().size(), 1);
  test::ExpectStrEq("FB2 recovery retains chapter label",
                   book.GetChapters()[0].title.c_str(), "Chapter 1");
  book.Close();
}

void TestOdtAndPlainAllocationRecovery() {
  const char *folder = getenv("TEST_ODT_READING_DIR");
  test::ExpectTrue("ODT fixture directory exists", folder != nullptr);
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  book.SetFolderName(folder);
  book.SetFileName("basic.odt");
  test::ExpectEq("real ODT opens", book_parser::Open(&book), 0);
  test::ExpectEq("short ODT occupies one page", book.GetPageCount(), 1);
  test::ExpectStrContains("ODT decodes entities", BookText(&book).c_str(),
                         "ODT first paragraph & decoded entity.");
  test::ExpectStrContains("ODT keeps final paragraph", BookText(&book).c_str(),
                         "ODT final paragraph.");
  test::ExpectEq("ODT retains both heading targets", (int)book.GetChapters().size(), 2);
  test::ExpectStrEq("ODT first heading", book.GetChapters()[0].title.c_str(), "Chapter One");
  test::ExpectStrEq("ODT nested heading", book.GetChapters()[1].title.c_str(), "Section Two");
  test::ExpectEq("ODT first level", book.GetChapters()[0].level, 0);
  test::ExpectEq("ODT nested level", book.GetChapters()[1].level, 1);
  book.Close();

  rejected_nothrow_allocations = 0;
  reject_nothrow_allocations = true;
  const int odt_rc = book_parser::Open(&book);
  reject_nothrow_allocations = false;
  test::ExpectEq("ODT state allocation failure propagates", odt_rc, XML_ERROR_NO_MEMORY);
  test::ExpectEqU("ODT handles failed state allocation once", rejected_nothrow_allocations, 1);
  test::ExpectEq("ODT allocation failure creates no pages", book.GetPageCount(), 0);
  test::ExpectTrue("ODT allocation failure creates no chapters", book.GetChapters().empty());
  book.Close();
  test::ExpectEq("ODT reopens after allocation failure", book_parser::Open(&book), 0);
  test::ExpectEq("ODT recovered chapter count", (int)book.GetChapters().size(), 2);
  book.Close();

  const std::string plain = "Plain recovery paragraph.\n";
  rejected_nothrow_allocations = 0;
  reject_nothrow_allocations = true;
  const int plain_rc = plain_parser::ParseBuffer(&book, plain, false);
  reject_nothrow_allocations = false;
  test::ExpectEq("plain state allocation failure propagates", plain_rc, 1);
  test::ExpectEqU("plain handles failed state allocation once", rejected_nothrow_allocations, 1);
  test::ExpectEq("plain allocation failure creates no pages", book.GetPageCount(), 0);
  test::ExpectTrue("plain allocation failure creates no chapters", book.GetChapters().empty());
  test::ExpectEq("plain parses after allocation failure", plain_parser::ParseBuffer(&book, plain, false), 0);
  test::ExpectStrContains("plain recovery preserves text", BookText(&book).c_str(),
                         "Plain recovery paragraph.");
  test::ExpectEq("plain recovery preserves short pagination", book.GetPageCount(), 1);
  test::ExpectTrue("plain recovery keeps heuristic navigation disabled", book.GetChapters().empty());
  book.Close();
}

void TestCoverHeaderPreflight() {
  const char *folder = getenv("TEST_COVER_READING_DIR");
  test::ExpectTrue("cover fixtures exist", folder != nullptr);
  BookContext ctx;
  Book book(ctx);
  struct ValidCase {
    const char *name;
    int limit, width, height;
    u16 pixel;
  };
  const ValidCase valid[] = {
      {"red.png", 2048, 2, 2, 0xf800},
      {"black.gif", 2048, 1, 1, 0x0000},
      {"green.jpg", 2048, 1, 1, 0x07e0},
      {"boundary.png", 2048, 85, 1, 0xf800},
      {"over-width.png", 0, 85, 1, 0xf800},
      {"over-height.png", -1, 1, 115, 0xf800},
  };
  auto read_image = [folder](const char *name) {
    const std::string path = std::string(folder) + "/" + name;
    FILE *fp = fopen(path.c_str(), "rb");
    test::ExpectTrue("image fixture opens", fp != nullptr);
    std::vector<unsigned char> data;
    int byte;
    while ((byte = fgetc(fp)) != EOF)
      data.push_back(static_cast<unsigned char>(byte));
    test::ExpectTrue("image fixture reads", !ferror(fp));
    test::ExpectEq("image fixture closes", fclose(fp), 0);
    return data;
  };
  for (const auto &row : valid) {
    const std::vector<unsigned char> data = read_image(row.name);
    cover_decode_calls = 0;
    test::ExpectTrue("accepted cover decodes", cover_decode_utils::DecodeImageToCoverThumb(
        &book, data.data(), data.size(), row.limit));
    test::ExpectEqU("accepted cover invokes actual decoder", cover_decode_calls, 1);
    test::ExpectEq("cover width preserved", book.coverWidth, row.width);
    test::ExpectEq("cover height preserved", book.coverHeight, row.height);
    for (int i = 0; i < book.coverWidth * book.coverHeight; ++i)
      test::ExpectEq("cover RGB565 pixels preserved", book.coverPixels[i], row.pixel);
  }
  u16 *const previous = book.coverPixels;
  const int previous_width = book.coverWidth;
  const int previous_height = book.coverHeight;
  const std::vector<u16> previous_pixels(previous, previous + previous_width * previous_height);
  const char *rejected[] = {"corrupt-header", "zero-width.gif", "zero-height.gif",
                            "over-width.png", "over-height.png", "corrupt-body.png"};
  for (const char *name : rejected) {
    const std::vector<unsigned char> data = read_image(name);
    cover_decode_calls = 0;
    test::ExpectFalse("rejected image leaves existing cover", cover_decode_utils::DecodeImageToCoverThumb(
        &book, data.data(), data.size()));
    test::ExpectTrue("rejection preserves cover ownership", book.coverPixels == previous);
    test::ExpectEq("rejection preserves width", book.coverWidth, previous_width);
    test::ExpectEq("rejection preserves height", book.coverHeight, previous_height);
    test::ExpectTrue("rejection preserves every previous pixel",
                    std::equal(previous_pixels.begin(), previous_pixels.end(), book.coverPixels));
    test::ExpectEqU("rejected header never enters decode; bad body still does",
                    cover_decode_calls, !strcmp(name, "corrupt-body.png") ? 1 : 0);
  }
}

void TestFb2NestedNavigation() {
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  book.SetFolderName(TEST_FIXTURES_DIR "/books");
  book.SetFileName("chapters.fb2");
  book.format = FORMAT_UNDEF;
  test::ExpectEq("nested FB2 opens", book_parser::Open(&book), 0);
  test::ExpectGt("nested FB2 creates pages", book.GetPageCount(), 0);
  const std::vector<ChapterEntry> &chapters = book.GetChapters();
  test::ExpectEq("nested FB2 has exactly three sections", (int)chapters.size(), 3);
  const char *titles[] = {"Chapter One", "Section 1.1", "Chapter Two"};
  const int levels[] = {0, 1, 0};
  for (size_t i = 0; i < 3; ++i) {
    test::ExpectStrEq("FB2 section label", chapters[i].title.c_str(), titles[i]);
    test::ExpectEq("FB2 section nesting resets", (int)chapters[i].level, levels[i]);
  }
  book.Close();
}

void TestOpenErrorMessages() {
  test::ExpectStrEq("corrupt books have short tag", BookOpenErrorTag(BOOK_ERR_CORRUPT), "corrupt_or_empty_book");
  test::ExpectStrEq("corrupt books have friendly text", DescribeBookOpenError(BOOK_ERR_CORRUPT), "error: corrupt or empty book");
  test::ExpectTrue("unknown errors have no short tag", BookOpenErrorTag(253) == nullptr);
  test::ExpectTrue("unknown errors fall back to numeric formatting", DescribeBookOpenError(253) == nullptr);
}

void TestEpubMetadataOpenCloseRecovery() {
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  book.SetFolderName(TEST_FIXTURES_DIR "/books");
  book.SetFileName("basic.epub");
  book.format = FORMAT_EPUB;

  test::ExpectEq("EPUB metadata dispatch", book_parser::Index(&book), 0);
  test::ExpectStrEq("indexed title", book.GetTitle(), "Basic EPUB Fixture");
  test::ExpectStrEq("indexed author", book.GetAuthor().c_str(), "3dslibris Test");
  test::ExpectEq("metadata indexing does not paginate", book.GetPageCount(), 0);
  test::ExpectTrue("metadata indexing does not create chapters", book.GetChapters().empty());
  test::ExpectEq("repeat metadata index", book_parser::Index(&book), 0);
  test::ExpectEq("repeat index still has no pages", book.GetPageCount(), 0);

  test::ExpectEq("fulltext dispatch after metadata", book_parser::Open(&book), 0);
  test::ExpectGt("fulltext dispatch paginates", book.GetPageCount(), 0);
  test::ExpectStrEq("metadata title survives fulltext dispatch", book.GetTitle(), "Basic EPUB Fixture");
  test::ExpectStrEq("metadata author survives fulltext dispatch", book.GetAuthor().c_str(), "3dslibris Test");
  const u16 pages = book.GetPageCount();
  const std::vector<ChapterEntry> chapters = book.GetChapters();
  test::ExpectEq("dispatch preserves both EPUB navigation labels", (int)chapters.size(), 2);
  test::ExpectStrEq("first EPUB label", chapters[0].title.c_str(), "Chapter One");
  test::ExpectStrEq("second EPUB label", chapters[1].title.c_str(), "Chapter Two");
  book.Close();
  test::ExpectEq("close clears paginated pages", book.GetPageCount(), 0);
  test::ExpectTrue("close clears navigation", book.GetChapters().empty());

  test::ExpectEq("reopen dispatch", book_parser::Open(&book), 0);
  test::ExpectEq("reopen does not append pages", book.GetPageCount(), pages);
  test::ExpectEq("reopen does not append chapters", (int)book.GetChapters().size(), 2);
  for (size_t i = 0; i < chapters.size(); ++i) {
    test::ExpectEq("reopen chapter page stable", book.GetChapters()[i].page, chapters[i].page);
    test::ExpectStrEq("reopen chapter label stable", book.GetChapters()[i].title.c_str(), chapters[i].title.c_str());
  }
  book.Close();
  book.SetFileName("missing-audit-dispatch.epub");
  test::ExpectTrue("missing EPUB dispatch returns error", book_parser::Open(&book) != 0);
  test::ExpectEq("failed dispatch exposes no stale pages", book.GetPageCount(), 0);
  test::ExpectTrue("failed dispatch exposes no stale chapters", book.GetChapters().empty());
  book.Close();
  book.SetFileName("basic.epub");
  test::ExpectEq("dispatch recovers after open failure", book_parser::Open(&book), 0);
  test::ExpectEq("recovery restores original page count", book.GetPageCount(), pages);
  book.Close();
}



struct CancelReporter : IStatusReporter {
  bool requested=false;
  void PrintStatus(const char *) override {}
  void PrintStatus(std::string) override {}
  bool ShouldAbortWork() const override { return requested; }
};
void TestCancelledOpenAndRecovery() {
  Text text; CancelReporter reporter; BookContext ctx;
  ctx.text=&text; ctx.status_reporter=&reporter; Book book(ctx);
  for (int flags=0; flags<4; ++flags) {
    reporter.requested=(flags & 1) != 0;
    book.ClearOpenAbortRequest();
    if (flags & 2) book.RequestAbortOpen();
    assert(open_cancel_poll::Poll(&book, &reporter, "test") == (flags != 0));
  }
  assert(open_cancel_poll::Poll(nullptr, &reporter, "test"));
  reporter.requested=false;
  assert(!open_cancel_poll::Poll(nullptr, &reporter, "test"));
  assert(!open_cancel_poll::Poll(nullptr, nullptr, "test"));
  const char *names[]={"basic.txt", "basic.fb2", "basic.epub"};
  book.SetFolderName(TEST_FIXTURES_DIR "/books");
  for (const char *name : names) {
    book.SetFileName(name); book.format=std::string(name) == "basic.epub" ? FORMAT_EPUB : FORMAT_UNDEF;
    book.PrepareForOpen(); book.RequestAbortOpen();
    test::ExpectEq("prepared parser respects book cancellation", book_parser::OpenPrepared(&book), BOOK_ERR_CANCELLED);
    book.Close(); assert(book.GetPageCount() == 0 && !book.IsOpenAbortRequested());
    reporter.requested=true; book.PrepareForOpen();
    test::ExpectEq("prepared parser respects application cancellation", book_parser::OpenPrepared(&book), BOOK_ERR_CANCELLED);
    book.Close(); reporter.requested=false;
    test::ExpectEq("normal open recovers after cancellation", book_parser::Open(&book), 0);
    assert(book.GetPageCount() > 0 && !BookText(&book).empty());
    book.Close();
  }
}

void TestCbzReadPageZoomCloseAndReopen() {
  const char *folder=getenv("TEST_CBZ_READING_DIR"); assert(folder);
  Text text; text.display.width=240; text.display.height=240;
  std::vector<u16> top(240 * 400 + 2, 0xbeef), bottom(240 * 320 + 2, 0xbeef);
  text.screenleft=top.data()+1; text.screenright=bottom.data()+1;
  BookContext ctx; ctx.text=&text; Book book(ctx);
  book.SetFolderName(folder); book.SetFileName("first.cbz");
  book.format=FORMAT_CBZ; // LibraryController supplies the format before opening.
  test::ExpectEq("CBZ opens through Book parser dispatch", book_parser::Open(&book), 0);
  assert(book.IsCbz() && book.GetPageCount() == 3);
  assert(book.GetChapters().size() == 1 && book.GetChapters()[0].page == 1);
  assert(book.GetChapters()[0].title == "Blue chapter");
  book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0xf800 && "first naturally sorted page is red");
  assert(book_renderer::ChangeFixedLayoutZoom(&book, 1));
  assert(book_renderer::TranslateFixedLayoutViewport(&book, .1f, .1f));
  book_renderer::SetFixedLayoutViewportInteraction(&book, true);
  book_renderer::DrawCurrentView(&book, &text);
  book_renderer::SetFixedLayoutViewportInteraction(&book, false);
  book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0xf800 && "zoom/pan retains requested page pixels");
  book.SetPosition(1); book_renderer::ResetFixedLayoutViewportForNavigation(&book);
  book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0x001f && "page turn replaces red with blue");
  text.printed_strings.clear();
  book.SetPosition(2); book_renderer::DrawCurrentView(&book, &text);
  assert(std::find(text.printed_strings.begin(), text.printed_strings.end(), "image decode failed") != text.printed_strings.end());
  book.SetPosition(0); book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0xf800);
  // Suspend-style reset must close the retained ZIP handle. A subsequent
  // read observes the missing path rather than the still-open old descriptor.
  book.ResetCbzTransientViewState(true);
  const std::string original=std::string(folder) + "/first.cbz", moved=original + ".moved";
  assert(rename(original.c_str(), moved.c_str()) == 0);
  text.printed_strings.clear();
  book.SetPosition(1); book_renderer::DrawCurrentView(&book, &text);
  assert(std::find(text.printed_strings.begin(), text.printed_strings.end(), "CBZ page unavailable") != text.printed_strings.end());
  assert(rename(moved.c_str(), original.c_str()) == 0);
  book.ResetCbzTransientViewState(true); book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0x001f);
  assert(top.front() == 0xbeef && top.back() == 0xbeef);
  assert(bottom.front() == 0xbeef && bottom.back() == 0xbeef);
  book.Close(); assert(book.GetPageCount() == 0 && book.GetChapters().empty());
  book.SetFileName("second.cbz");
  assert(book_parser::Open(&book) == 0 && book.GetPageCount() == 1 && book.GetChapters().empty());
  book.SetPosition(0); book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0x07e0 && "another archive cannot reuse earlier page pixels");
  book.Close(); book.SetFileName("first.cbz");
  assert(book_parser::Open(&book) == 0 && book.GetPageCount() == 3 && book.GetChapters().size() == 1);
  book.SetPosition(0); book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0xf800);
  book.Close(); book.SetFileName("missing.cbz");
  assert(book_parser::Open(&book) != 0 && book.GetPageCount() == 0);
  book.Close();
}

void TestDecodeCp1252() {
  const std::string raw = std::string("caf") + "\xE9" + " y " + "\x97" + " fin";
  bool used_utf8_guess = true;
  bool used_legacy_guess = true;
  const std::string decoded =
      mobi_text_decode::DecodeBytesToUtf8(raw, 1252, &used_utf8_guess,
                                          &used_legacy_guess);

  test::ExpectStrEq("cp1252 decode", decoded.c_str(), "caf\xC3\xA9 y \xE2\x80\x94 fin");
  test::ExpectFalse("cp1252 used_utf8_guess", used_utf8_guess);
  test::ExpectFalse("cp1252 used_legacy_guess", used_legacy_guess);
}

std::vector<std::string> CacheFiles(const std::string &folder) {
  DIR *dir = opendir(folder.c_str());
  test::ExpectTrue("cache directory is readable", dir != nullptr);
  std::vector<std::string> files;
  while (dirent *entry = readdir(dir)) {
    if (entry->d_name[0] != '.')
      files.push_back(folder + "/" + entry->d_name);
  }
  closedir(dir);
  return files;
}

std::string ReadFileBytes(const std::string &path) {
  FILE *fp = fopen(path.c_str(), "rb");
  test::ExpectTrue("source fixture exists", fp != nullptr);
  std::string data;
  char buffer[4096];
  size_t length;
  while ((length = fread(buffer, 1, sizeof(buffer), fp)) != 0)
    data.append(buffer, length);
  test::ExpectFalse("fixture read has no I/O error", ferror(fp));
  test::ExpectEq("fixture read closes", fclose(fp), 0);
  return data;
}

void WriteFileBytes(const std::string &path, const std::string &data) {
  FILE *fp = fopen(path.c_str(), "wb");
  test::ExpectTrue("fixture opens for replacement", fp != nullptr);
  test::ExpectTrue("fixture replacement writes all bytes",
                   fwrite(data.data(), 1, data.size(), fp) == data.size());
  test::ExpectEq("fixture replacement closes", fclose(fp), 0);
}

typedef std::vector<std::vector<u32>> PageData;

PageData CapturePageData(Book *book) {
  PageData data;
  for (int i = 0; i < book->GetPageCount(); ++i) {
    Page *page = book->GetPage(i);
    data.push_back(std::vector<u32>());
    if (page->GetLength() > 0)
      data.back().assign(page->GetBuffer(), page->GetBuffer() + page->GetLength());
  }
  return data;
}

PageData ImagePagesWithoutResourceIds(Book *book) {
  PageData data = CapturePageData(book);
  for (auto &page : data) {
    for (size_t i = 0; i + 1 < page.size(); ++i) {
      if (page[i] == TEXT_IMAGE)
        page[++i] = 0;
    }
  }
  return data;
}

struct ImageRenderSnapshot {
  std::vector<std::vector<u16>> frames;
  std::vector<std::string> glyphs;
};

ImageRenderSnapshot RenderImagePages(Book *book, Text *text) {
  ImageRenderSnapshot result;
  std::vector<u16> left(400 * 400 + 2), right(400 * 400 + 2);
  text->screenleft = left.data() + 1;
  text->screenright = right.data() + 1;
  std::map<int, int> red_rows;
  int painted_screens[2] = {0, 0};
  for (int page = 0; page < book->GetPageCount(); ++page) {
    std::fill(left.begin(), left.end(), 0);
    std::fill(right.begin(), right.end(), 0);
    left.front() = left.back() = right.front() = right.back() = 0xbeef;
    text->screen = text->screenleft;
    text->rendered_glyphs.clear();
    book->GetPage(page)->Draw(text);
    test::ExpectTrue("inline draw respects framebuffer guards",
        left.front() == 0xbeef && left.back() == 0xbeef &&
        right.front() == 0xbeef && right.back() == 0xbeef);
    for (int screen = 0; screen < 2; ++screen) {
      const u16 *pixels = screen == 0 ? text->screenleft : text->screenright;
      result.frames.emplace_back(pixels, pixels + 400 * 400);
      for (int y = 0; y < 400; ++y) {
        int red = 0;
        for (int x = 0; x < 400; ++x)
          if (pixels[y * 400 + x] == 0xf800) ++red;
        if (red) { ++red_rows[red]; ++painted_screens[screen]; }
      }
    }
    for (const auto &glyph : text->rendered_glyphs) {
      char position[96];
      snprintf(position, sizeof(position), "%d:%d:%d:%d:%u", page,
          glyph.screen == text->screenleft ? 0 : 1, glyph.x, glyph.y,
          (unsigned)glyph.codepoint);
      result.glyphs.push_back(position);
    }
  }
  // Three occurrences of each 160x32 red image: native, 40x8, and 80x16.
  // These literal pixel counts are independent of layout/planning helpers.
  test::ExpectEq("40px occurrences retain eight red rows each", red_rows[40], 24);
  test::ExpectEq("80px occurrences retain sixteen red rows each", red_rows[80], 48);
  test::ExpectEq("unconstrained occurrences retain native width", red_rows[160], 96);
  test::ExpectEq("no unexpected scaled image width", (int)red_rows.size(), 3);
  test::ExpectTrue("actual images reach both physical screens", painted_screens[0] && painted_screens[1]);
  test::ExpectFalse("following text is drawn", result.glyphs.empty());
  text->screen = text->screenleft = text->screenright = nullptr;
  return result;
}

bool LoadImagePageCache(Book *book, const std::string &path) {
  const BookParseDeps deps = BuildBookParseDeps(book);
  return epub_page_cache::TryLoad(book, path.c_str(), deps.layout.pixel_size,
      deps.layout.linespacing, deps.paragraph_spacing, deps.paragraph_indent,
      deps.orientation, deps.layout.margin_left, deps.layout.margin_right,
      deps.layout.margin_top, deps.layout.margin_bottom,
      deps.regular_font_path.c_str());
}

void TestRepeatedInlineImageWidthsAndCachedDraw() {
  const char *folder = getenv("TEST_INLINE_IMAGE_READING_DIR");
  test::ExpectTrue("inline-image fixture directory configured", folder != nullptr);
  for (unsigned char orientation = 0; orientation < 3; ++orientation) {
    Text text;
    text.capture_rendered_text = true;
    text.landscape = orientation == 2;
    text.display.width = text.landscape ? 400 : 240;
    text.display.height = 400; // The stub's buffer stride; actual logical height comes from orientation.
    BookContext ctx; ctx.text = &text; ctx.orientation = &orientation;
    const std::string cache_dir = std::string(folder) + "/cache/" + std::to_string(orientation);
    test::ExpectEq("create isolated image cache", mkdir(cache_dir.c_str(), 0700), 0);
    epub_page_cache::SetCacheDirForTest(cache_dir.c_str());
    Book separate(ctx); separate.SetFolderName(folder); separate.SetFileName("separate.epub");
    separate.format = FORMAT_EPUB; // LibraryController supplies the format before dispatch.
    test::ExpectEq("open independent-image control", book_parser::Open(&separate), 0);
    test::ExpectEqU("control has three identical-byte resources", separate.GetInlineImageCount(), 3);
    const PageData expected_pages = ImagePagesWithoutResourceIds(&separate);
    const std::vector<ChapterEntry> chapters = separate.GetChapters();
    const ImageRenderSnapshot expected = RenderImagePages(&separate, &text);
    Book shared(ctx); shared.SetFolderName(folder); shared.SetFileName("shared.epub");
    shared.format = FORMAT_EPUB;
    test::ExpectEq("open repeated-image fixture", book_parser::Open(&shared), 0);
    test::ExpectEqU("repeated src still deduplicates resource", shared.GetInlineImageCount(), 1);
    for (int screen = 0; screen < 2; ++screen) {
      const int authored[] = {40, 80, 0};
      const int expected_width[] = {40, 80, 160};
      const int expected_height[] = {8, 16, 32};
      for (int i = 0; i < 3; ++i) {
        InlineImageLayoutPlan plan{};
        test::ExpectTrue("plan real metadata for this occurrence",
            shared.PlanInlineImageLayout(&text, 0, screen, text.margin.left,
                text.margin.top + text.GetHeight(), false,
                INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH, &plan, authored[i]));
        test::ExpectEq("occurrence width does not inherit resource history", plan.draw_width, expected_width[i]);
        test::ExpectEq("occurrence preserves aspect ratio", plan.draw_height, expected_height[i]);
      }
    }
    test::ExpectTrue("resource aliasing cannot change pagination", ImagePagesWithoutResourceIds(&shared) == expected_pages);
    const ImageRenderSnapshot cold = RenderImagePages(&shared, &text);
    test::ExpectTrue("aliasing cannot change image pixels or positions", cold.frames == expected.frames);
    test::ExpectTrue("aliasing cannot move following text", cold.glyphs == expected.glyphs);
    test::ExpectEq("all fixture TOC targets survive", (int)shared.GetChapters().size(), 3);
    for (size_t i = 0; i < chapters.size(); ++i) {
      test::ExpectEq("TOC pages independent of resource sharing", shared.GetChapters()[i].page, chapters[i].page);
      test::ExpectStrEq("TOC labels preserved", shared.GetChapters()[i].title.c_str(), chapters[i].title.c_str());
    }
    const PageData cold_pages = CapturePageData(&shared);
    shared.Close();
    const std::vector<std::string> caches = CacheFiles(cache_dir);
    test::ExpectEq("close saves one real image page cache", (int)caches.size(), 1);
    const std::string path = std::string(folder) + "/shared.epub";
    Book warm(ctx); warm.SetFolderName(folder); warm.SetFileName("shared.epub"); warm.format = FORMAT_EPUB;
    test::ExpectTrue("load actual image page cache without parsing", LoadImagePageCache(&warm, path));
    test::ExpectTrue("cache preserves per-occurrence tokens", CapturePageData(&warm) == cold_pages);
    test::ExpectEqU("cache restores deduplicated image registry", warm.GetInlineImageCount(), 1);
    const ImageRenderSnapshot loaded = RenderImagePages(&warm, &text);
    test::ExpectTrue("warm cache draws identical image pixels", loaded.frames == cold.frames);
    test::ExpectTrue("warm cache retains following text positions", loaded.glyphs == cold.glyphs);
    warm.Close();
    std::string obsolete = ReadFileBytes(caches[0]);
    obsolete[4] = 17; obsolete[5] = 0; // Previous pagination could inherit another occurrence's width.
    WriteFileBytes(caches[0], obsolete);
    test::ExpectFalse("reject old EPUB pagination after width correction", LoadImagePageCache(&warm, path));
    test::ExpectTrue("obsolete image pagination cache is evicted", CacheFiles(cache_dir).empty());
    test::ExpectEq("reopen reparses obsolete pagination", book_parser::Open(&warm), 0);
    test::ExpectTrue("reparse recreates corrected tokens", CapturePageData(&warm) == cold_pages);
    warm.Close(); separate.Close();
    for (const std::string &cache : CacheFiles(cache_dir))
      test::ExpectEq("remove isolated image cache", remove(cache.c_str()), 0);
    epub_page_cache::SetCacheDirForTest(nullptr);
    printf("PASS: repeated inline image widths, both screens, orientation %u and cache\n", (unsigned)orientation);
  }
}

void ExpectMobiContent(Book *book) {
  const std::string content = BookText(book);
  size_t cursor = 0;
  for (unsigned word = 1; word <= 4000; ++word) {
    char expected[16];
    snprintf(expected, sizeof(expected), "TOKEN%04u", word);
    const size_t found = content.find(expected, cursor);
    test::ExpectTrue(expected, found != std::string::npos);
    test::ExpectTrue("every MOBI word appears exactly once",
                     content.find(expected, found + 1) == std::string::npos);
    cursor = found + 9;
  }
  test::ExpectStrContains("MOBI pagination reaches the end", content.c_str(), "MOBIEND");
  test::ExpectStrContains("MOBI decodes UTF-8 and entities", content.c_str(), "café & tea");
  test::ExpectTrue("MOBI excludes script text", content.find("HIDDEN-MOBI") == std::string::npos);
  test::ExpectTrue("MOBI consumes HTML", content.find("<h1>") == std::string::npos);
  test::ExpectGt("MOBI fixture actually spans pages", book->GetPageCount(), 1);
  const char *labels[] = {"Chapter One", "Chapter Two", "Chapter Three", "Chapter Four"};
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  test::ExpectEq("real MOBI hooks retain all TOC entries", (int)chapters.size(), 4);
  for (size_t i = 0; i < 4; ++i) {
    test::ExpectStrEq("MOBI chapter label", chapters[i].title.c_str(), labels[i]);
    test::ExpectTrue("MOBI chapter targets a readable page", chapters[i].page < book->GetPageCount());
    if (i)
      test::ExpectTrue("MOBI chapters follow the source order", chapters[i].page > chapters[i - 1].page);
  }
}

struct MobiBufferReporter : CancelReporter {
  std::vector<std::string> buffers;
  void PrintStatus(const char *line) override {
    if (line && std::string(line).find("MOBI buffer:") != std::string::npos)
      buffers.push_back(line);
  }
  void PrintStatus(std::string line) override { PrintStatus(line.c_str()); }
#ifdef DSLIBRIS_DEBUG
  size_t Size(const char *phase, const char *name) const {
    const std::string prefix = std::string("MOBI buffer: phase=") + phase + " name=" + name + " ";
    for (const auto &line : buffers) {
      const size_t pos = line.find(prefix);
      if (pos == std::string::npos) continue;
      unsigned long size = 0, capacity = 0;
      test::ExpectEq("diagnostic supplies size and capacity",
          std::sscanf(line.c_str() + pos + prefix.size(), "size=%lu capacity=%lu", &size, &capacity), 2);
      test::ExpectTrue("buffer capacity covers its live bytes", capacity >= size);
      if (std::string(phase).find("released") != std::string::npos)
        test::ExpectEqU("released capacity matches an empty string", capacity, std::string().capacity());
      return size;
    }
    test::ExpectTrue(prefix.c_str(), false);
    return 0;
  }
#endif
};

void TestMobiParseCloseCachedReopenAndRecovery() {
  const char *folder = getenv("TEST_MOBI_READING_DIR");
  test::ExpectTrue("MOBI fixture folder supplied", folder != nullptr);
  const std::string cache_dir = std::string(folder) + "/cache";
  mobi_page_cache::SetCacheDirForTest(cache_dir.c_str());
  const char *names[] = {"raw.mobi", "palmdoc.mobi"};
  for (const char *name : names) {
    Text text;
    MobiBufferReporter reporter;
    BookContext ctx;
    ctx.text = &text;
    ctx.status_reporter = &reporter;
    const std::string path = std::string(folder) + "/" + name;
    const std::string source = ReadFileBytes(path);
    PageData pages;
    std::vector<ChapterEntry> chapters;
    {
      Book book(ctx);
      book.SetFolderName(folder);
      book.SetFileName(name);
      test::ExpectEq(name, book_parser::Open(&book), 0);
      ExpectMobiContent(&book);
#ifdef DSLIBRIS_DEBUG
      // Independent PalmDOC header oracle: first record offset, then text length.
      auto be32 = [&source](size_t offset) {
        test::ExpectTrue("fixture header bounds", offset + 4 <= source.size());
        unsigned value = 0;
        for (size_t i = 0; i < 4; ++i) value = (value << 8) | (unsigned char)source[offset + i];
        return value;
      };
      const size_t text_bytes = be32(be32(78) + 4);
      test::ExpectEqU("raw diagnostic matches file bytes", reporter.Size("source", "raw"), source.size());
      test::ExpectEqU("merged diagnostic matches declared text", reporter.Size("merged", "merged"), text_bytes);
      test::ExpectEqU("UTF-8 fixture is not transcoded", reporter.Size("utf8", "utf8"), text_bytes);
      const size_t cleaned = reporter.Size("cleaned", "text");
      test::ExpectTrue("markup extraction produces smaller live text", cleaned > 0 && cleaned < text_bytes);
      test::ExpectEqU("merged storage released", reporter.Size("merged_released", "merged"), 0);
      test::ExpectEqU("raw storage released after TOC", reporter.Size("raw_released", "raw"), 0);
#else
      test::ExpectTrue("release has no buffer diagnostics", reporter.buffers.empty());
#endif
      pages = CapturePageData(&book);
      chapters = book.GetChapters();
      test::ExpectTrue("cold MOBI open schedules a real cache save", book.HasPendingMobiPageCacheSave());
      test::ExpectTrue("cold open leaves cache write off the critical path", CacheFiles(cache_dir).empty());
      book.Close();
      test::ExpectEq("MOBI close releases pages", book.GetPageCount(), 0);
      test::ExpectTrue("MOBI close releases chapters", book.GetChapters().empty());
    }
    const std::vector<std::string> caches = CacheFiles(cache_dir);
    test::ExpectEq("Close saves one MOBI cache artifact", (int)caches.size(), 1);
    struct stat original;
    test::ExpectEq("source attributes readable", stat(path.c_str(), &original), 0);
    // Preserve the source identity used by the cache key but make decoding
    // impossible. A successful warm open must therefore come from the cache.
    WriteFileBytes(path, std::string(source.size(), '\0'));
    const utimbuf times = {original.st_atime, original.st_mtime};
    test::ExpectEq("preserve fixture timestamp", utime(path.c_str(), &times), 0);
    Book reopened(ctx);
    reopened.SetFolderName(folder);
    reopened.SetFileName(name);
    test::ExpectEq("cached open succeeds with source decoding unavailable", book_parser::Open(&reopened), 0);
    ExpectMobiContent(&reopened);
    test::ExpectTrue("cache preserves every page codepoint", CapturePageData(&reopened) == pages);
    for (size_t i = 0; i < chapters.size(); ++i)
      test::ExpectEq("cache preserves chapter targets", reopened.GetChapters()[i].page, chapters[i].page);
    test::ExpectFalse("cache hit does not schedule reparsed content", reopened.HasPendingMobiPageCacheSave());
    reopened.Close();
    test::ExpectEq("remove cache for negative control", remove(caches[0].c_str()), 0);
    test::ExpectTrue("same invalid source fails without the cache", book_parser::Open(&reopened) != 0);
    test::ExpectEq("rejected source creates no pages", reopened.GetPageCount(), 0);
    test::ExpectFalse("failed source cannot schedule a cache", reopened.HasPendingMobiPageCacheSave());
    reopened.Close();
    WriteFileBytes(path, source);
    test::ExpectEq("restore fixture timestamp", utime(path.c_str(), &times), 0);
    test::ExpectEq("MOBI recovers after invalid source", book_parser::Open(&reopened), 0);
    ExpectMobiContent(&reopened);
    reopened.Close();
    const std::vector<std::string> restored_caches = CacheFiles(cache_dir);
    test::ExpectEq("recovery recreates one cache", (int)restored_caches.size(), 1);
    const std::string saved_cache = ReadFileBytes(restored_caches[0]);
    WriteFileBytes(restored_caches[0], saved_cache.substr(0, saved_cache.size() / 2));
    test::ExpectEq("partial cache falls back to the real MOBI parser", book_parser::Open(&reopened), 0);
    ExpectMobiContent(&reopened);
    test::ExpectTrue("fallback discards partially loaded pages", CapturePageData(&reopened) == pages);
    test::ExpectTrue("cache fallback schedules replacement", reopened.HasPendingMobiPageCacheSave());
    reopened.Close();
    test::ExpectTrue("Close replaces the partial cache", ReadFileBytes(restored_caches[0]) == saved_cache);
    text.pixelsize += 4;
    test::ExpectEq("MOBI opens with a different layout", book_parser::Open(&reopened), 0);
    ExpectMobiContent(&reopened);
    test::ExpectTrue("different layout requires new cache save", reopened.HasPendingMobiPageCacheSave());
    test::ExpectGt("larger font actually repaginates", reopened.GetPageCount(), (int)pages.size());
    reopened.Close();
    for (const std::string &cache : CacheFiles(cache_dir))
      test::ExpectEq("remove isolated MOBI cache", remove(cache.c_str()), 0);
    reopened.SetFileName((std::string("truncated-") + name).c_str());
    test::ExpectTrue("truncated MOBI is rejected through dispatch", book_parser::Open(&reopened) != 0);
    test::ExpectFalse("truncated MOBI cannot schedule cache save", reopened.HasPendingMobiPageCacheSave());
    reopened.Close();
    test::ExpectTrue("failed MOBI close leaves no cache", CacheFiles(cache_dir).empty());
    printf("PASS: real MOBI %s, %lu pages, 4000 ordered words, four chapters, cache and recovery\n",
           name, (unsigned long)pages.size());
  }
  mobi_page_cache::SetCacheDirForTest(nullptr);
}

void ExpectClosedReadingState(Book *book) {
  test::ExpectEq("Close releases pages after interruption", book->GetPageCount(), 0);
  test::ExpectTrue("Close releases chapters after interruption", book->GetChapters().empty());
  test::ExpectEqU("Close releases links after interruption", book->GetInlineLinkHrefCount(), 0);
  test::ExpectEqU("Close releases anchors after interruption", book->GetChapterAnchorCount(), 0);
  test::ExpectTrue("Close releases document navigation", book->GetChapterDocStartPages().empty());
  test::ExpectFalse("Close clears pending EPUB cache", book->HasPendingEpubPageCacheSave());
}

void ExpectCompleteRecoveryEpub(Book *book, bool expect_middle = true) {
  const std::string content = BookText(book);
  test::ExpectStrContains("EPUB includes start of first document", content.c_str(), "FIRST-START");
  test::ExpectStrContains("EPUB includes end of first document", content.c_str(), "FIRST-END");
  test::ExpectStrContains("EPUB reaches final spine document", content.c_str(), "FINAL-SPINE");
  test::ExpectStrEq("EPUB metadata survives recovery", book->GetTitle(), "EPUB Recovery Fixture");
  if (expect_middle) {
    test::ExpectStrContains("EPUB includes the middle document", content.c_str(), "MIDDLE-SPINE");
  }
  test::ExpectEq("EPUB retains complete navigation", (int)book->GetChapters().size(), 3);
  bool first_found = false, last_found = false;
  for (const ChapterEntry &chapter : book->GetChapters()) {
    test::ExpectTrue("recovered EPUB chapter targets a readable page", chapter.page < book->GetPageCount());
    first_found |= chapter.title == "first chapter";
    last_found |= chapter.title == "last chapter";
  }
  test::ExpectTrue("EPUB keeps navigation for the valid documents", first_found && last_found);
  test::ExpectGt("EPUB registers real inline links", book->GetInlineLinkHrefCount(), 0);
  test::ExpectGt("EPUB registers real anchors", (int)book->GetChapterAnchorCount(), 0);
}

struct SpineCancellation : IStatusReporter {
  Book *book = nullptr;
  int mode = 0;
  bool enabled = true;
  bool requested = false;
  bool first_completed = false;
  void PrintStatus(const char *) override {}
  void PrintStatus(std::string) override {}
  bool ShouldAbortWork() const override {
    return enabled && (requested || (mode == 2 && book && book->GetPageCount() > 0));
  }
  static void OnProgress(unsigned done, unsigned total, void *user_data) {
    SpineCancellation *self = static_cast<SpineCancellation *>(user_data);
    if (!self->enabled || done != 1)
      return;
    test::ExpectEq("cancellation fixture contains three spine documents", total, 3);
    self->first_completed = true;
    if (self->mode == 0)
      self->book->RequestAbortOpen();
    else if (self->mode == 1)
      self->requested = true;
  }
};

void TestEpubInterruptedSpineAndRecovery() {
  const char *folder = getenv("TEST_EPUB_RECOVERY_DIR");
  test::ExpectTrue("EPUB recovery folder supplied", folder != nullptr);
  const std::string cache_dir = std::string(folder) + "/cache-cancel";
  epub_page_cache::SetCacheDirForTest(cache_dir.c_str());
  for (int mode = 0; mode < 3; ++mode) {
    Text text;
    SpineCancellation cancellation;
    cancellation.mode = mode;
    BookContext ctx;
    ctx.text = &text;
    ctx.status_reporter = &cancellation;
    ctx.on_spine_progress = SpineCancellation::OnProgress;
    ctx.on_spine_progress_user_data = &cancellation;
    Book book(ctx);
    cancellation.book = &book;
    book.SetFolderName(folder);
    book.SetFileName("valid.epub");
    book.format = FORMAT_EPUB;
    test::ExpectEq("interrupted EPUB returns cancellation", book_parser::Open(&book), BOOK_ERR_CANCELLED);
    test::ExpectGt("cancellation occurs after actual content was parsed", book.GetPageCount(), 0);
    const std::string partial = BookText(&book);
    test::ExpectStrContains("cancelled book reached the first document", partial.c_str(), "FIRST-START");
    test::ExpectTrue("cancelled book never reaches the next document", partial.find("MIDDLE-SPINE") == std::string::npos);
    test::ExpectTrue("cancelled book never reaches the last document", partial.find("FINAL-SPINE") == std::string::npos);
    test::ExpectTrue("cancellation happens at the intended boundary", cancellation.first_completed == (mode != 2));
    if (mode == 2)
      test::ExpectTrue("XML streaming aborts before the first document finishes", partial.find("FIRST-END") == std::string::npos);
    else
      test::ExpectStrContains("progress cancellation follows completed document", partial.c_str(), "FIRST-END");
    test::ExpectFalse("partial EPUB cannot schedule a cache save", book.HasPendingEpubPageCacheSave());
    book.Close();
    ExpectClosedReadingState(&book);
    test::ExpectTrue("cancelled EPUB Close writes no partial cache", CacheFiles(cache_dir).empty());

    cancellation.enabled = false;
    // Reuse the Book through a different parser before returning to this EPUB.
    book.SetFolderName(TEST_FIXTURES_DIR "/books");
    book.SetFileName("basic.txt");
    book.format = FORMAT_UNDEF;
    test::ExpectEq("TXT opens after interrupted EPUB", book_parser::Open(&book), 0);
    const std::string other = BookText(&book);
    test::ExpectStrContains("recovery uses the requested TXT", other.c_str(), "tiny TXT fixture");
    test::ExpectTrue("TXT contains no interrupted EPUB content", other.find("FIRST-START") == std::string::npos);
    book.Close();
    book.SetFolderName(folder);
    book.SetFileName("valid.epub");
    book.format = FORMAT_EPUB;
    test::ExpectEq("same EPUB opens completely after cancellation", book_parser::Open(&book), 0);
    ExpectCompleteRecoveryEpub(&book);
    test::ExpectTrue("complete recovery can schedule a cache", book.HasPendingEpubPageCacheSave());
    book.Close();
    ExpectClosedReadingState(&book);
    const std::vector<std::string> caches = CacheFiles(cache_dir);
    test::ExpectEq("complete recovery creates one EPUB cache", (int)caches.size(), 1);
    test::ExpectEq("remove isolated recovery cache", remove(caches[0].c_str()), 0);
    printf("PASS: EPUB cancellation mode %d, partial-cache rejection and cross-format recovery\n", mode);
  }
  epub_page_cache::SetCacheDirForTest(nullptr);
}

void TestEpubXmlRecoveryAndZipFailure() {
  const char *folder = getenv("TEST_EPUB_RECOVERY_DIR");
  test::ExpectTrue("EPUB recovery folder supplied", folder != nullptr);
  const char *variants[] = {"valid", "badxml", "badcrc"};
  for (const char *variant : variants) {
    const std::string cache_dir = std::string(folder) + "/cache-" + variant;
    epub_page_cache::SetCacheDirForTest(cache_dir.c_str());
    Text text;
    CancelReporter reporter;
    BookContext ctx;
    ctx.text = &text;
    ctx.status_reporter = &reporter;
    Book book(ctx);
    book.SetFolderName(folder);
    book.SetFileName((std::string(variant) + ".epub").c_str());
    book.format = FORMAT_EPUB;
    const u8 result = book_parser::Open(&book);
    if (std::string(variant) == "badcrc") {
      test::ExpectEq("ZIP checksum failure rejects the opening", result, (u8)UNZ_CRCERROR);
      const std::string partial = BookText(&book);
      test::ExpectStrContains("CRC failure occurs after the valid first document", partial.c_str(), "FIRST-END");
      test::ExpectTrue("CRC failure stops before the last document", partial.find("FINAL-SPINE") == std::string::npos);
      test::ExpectFalse("CRC failure cannot schedule a cache", book.HasPendingEpubPageCacheSave());
      book.Close();
      ExpectClosedReadingState(&book);
      test::ExpectTrue("CRC failure writes no partial cache", CacheFiles(cache_dir).empty());
      book.SetFileName("valid.epub");
      test::ExpectEq("valid EPUB opens after fatal ZIP failure", book_parser::Open(&book), 0);
    } else {
      test::ExpectEq("valid ZIP with recoverable XHTML opens", result, 0);
    }
    ExpectCompleteRecoveryEpub(&book, std::string(variant) != "badxml");
    book.Close();
    ExpectClosedReadingState(&book);
    for (const std::string &cache : CacheFiles(cache_dir))
      test::ExpectEq("remove isolated archive test cache", remove(cache.c_str()), 0);
    printf("PASS: EPUB %s, XML/ZIP error distinction and recovery\n", variant);
  }
  epub_page_cache::SetCacheDirForTest(nullptr);
}

void TestUtf8DetectionAndPassThrough() {
  const std::string utf8 = "\xC2\xA1Hola, se\xC3\xB1or!";

  bool used_utf8_guess = false;
  bool used_legacy_guess = false;
  const std::string detected =
      mobi_text_decode::DecodeBytesToUtf8(utf8, 0, &used_utf8_guess,
                                          &used_legacy_guess);
  test::ExpectStrEq("unknown encoding utf8 passthrough", detected.c_str(),
                    utf8.c_str());
  test::ExpectTrue("unknown encoding used_utf8_guess", used_utf8_guess);
  test::ExpectFalse("unknown encoding used_legacy_guess", used_legacy_guess);

  used_utf8_guess = true;
  used_legacy_guess = true;
  const std::string explicit_utf8 =
      mobi_text_decode::DecodeBytesToUtf8(utf8, 65001, &used_utf8_guess,
                                          &used_legacy_guess);
  test::ExpectStrEq("explicit utf8 passthrough", explicit_utf8.c_str(),
                    utf8.c_str());
  test::ExpectFalse("explicit utf8 used_utf8_guess", used_utf8_guess);
  test::ExpectFalse("explicit utf8 used_legacy_guess", used_legacy_guess);
}

void TestDecodeIso88591() {
  const std::string raw = std::string("Ol") + "\xE1" + " mundo";
  bool used_utf8_guess = true;
  bool used_legacy_guess = true;
  const std::string decoded =
      mobi_text_decode::DecodeBytesToUtf8(raw, 28591, &used_utf8_guess,
                                          &used_legacy_guess);

  test::ExpectStrEq("iso-8859-1 decode", decoded.c_str(), "Ol\xC3\xA1 mundo");
  test::ExpectFalse("iso-8859-1 used_utf8_guess", used_utf8_guess);
  test::ExpectFalse("iso-8859-1 used_legacy_guess", used_legacy_guess);
}

void TestApplyEmbeddedTitleFromMetadata() {
  mobi_parser_core::MobiHeaderInfo header;
  header.encoding = 1252;
  header.mobi_full_name_off = 12;
  const std::string raw_title = std::string("  T") + "\xED" + "tulo   de   prueba  ";
  header.mobi_full_name_len = static_cast<u32>(raw_title.size());

  std::string rec0(80, '\0');
  rec0.replace(header.mobi_full_name_off, raw_title.size(), raw_title);

  std::string raw = rec0;
  raw.append("NEXT_RECORD");

  header.offsets.push_back(0);
  header.offsets.push_back(static_cast<u32>(rec0.size()));
  header.offsets.push_back(static_cast<u32>(raw.size()));

  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  mobi_text_decode::ApplyEmbeddedTitle(&book, raw, header);
  test::ExpectStrEq("embedded title extracted and normalized", book.GetTitle(),
                    "T\xC3\xADtulo de prueba");
}

void TestUnknownEncodingWithMalformedBytes() {
  const std::string malformed = std::string("A") + "\xFF\x80";
  bool used_utf8_guess = false;
  bool used_legacy_guess = false;

  const std::string decoded =
      mobi_text_decode::DecodeBytesToUtf8(malformed, 0xFFFFFFFFu,
                                          &used_utf8_guess,
                                          &used_legacy_guess);

  test::ExpectTrue("malformed unknown picks legacy guess", used_legacy_guess);
  test::ExpectFalse("malformed unknown utf8 guess", used_utf8_guess);
  test::ExpectStrContains("malformed unknown keeps ascii", decoded.c_str(), "A");
  test::ExpectStrContains("malformed unknown maps cp1252 euro", decoded.c_str(),
                          "\xE2\x82\xAC");
}

void TestNullAndEmptyInputHandling() {
  bool used_utf8_guess = true;
  bool used_legacy_guess = true;
  const std::string decoded =
      mobi_text_decode::DecodeBytesToUtf8("", 1252, &used_utf8_guess,
                                          &used_legacy_guess);
  test::ExpectStrEq("empty decode", decoded.c_str(), "");
  test::ExpectFalse("empty decode utf8 guess", used_utf8_guess);
  test::ExpectFalse("empty decode legacy guess", used_legacy_guess);

  mobi_parser_core::MobiHeaderInfo header;
  header.mobi_full_name_len = 10;
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  book.SetTitle("unchanged");
  mobi_text_decode::ApplyEmbeddedTitle(nullptr, "", header);
  mobi_text_decode::ApplyEmbeddedTitle(&book, "", header);
  test::ExpectStrEq("null/empty title apply leaves title unchanged",
                    book.GetTitle(), "unchanged");
}

} // namespace

int main() {
  TestRepeatedInlineImageWidthsAndCachedDraw();
  TestCoverHeaderPreflight();
  TestOdtAndPlainAllocationRecovery();
  TestXmlAllocationFailureAndRecovery();
  TestDispatchContentAndCloseBetweenFormats();
  TestFb2NestedNavigation();
  TestOpenErrorMessages();
  TestEpubMetadataOpenCloseRecovery();
  TestCbzReadPageZoomCloseAndReopen();
  TestCancelledOpenAndRecovery();
  TestMobiParseCloseCachedReopenAndRecovery();
  TestEpubInterruptedSpineAndRecovery();
  TestEpubXmlRecoveryAndZipFailure();
  TestDecodeCp1252();
  TestUtf8DetectionAndPassThrough();
  TestDecodeIso88591();
  TestApplyEmbeddedTitleFromMetadata();
  TestUnknownEncodingWithMalformedBytes();
  TestNullAndEmptyInputHandling();
  return 0;
}

#include "book/book.h"
#include "book/book_context.h"
#include "book/book_parser.h"
#include "formats/common/page_text_extract_utils.h"
#include "formats/common/book_error.h"
#include "formats/mobi/mobi_text_decode.h"
#include "shared/app_flow_utils.h"
#include "shared/open_cancel_poll.h"
#include "shared/status_reporter.h"
#include "ui/text.h"
#include "test_assert.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "tests/fixtures"
#endif

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
  TestDispatchContentAndCloseBetweenFormats();
  TestFb2NestedNavigation();
  TestOpenErrorMessages();
  TestEpubMetadataOpenCloseRecovery();
  TestCancelledOpenAndRecovery();
  TestDecodeCp1252();
  TestUtf8DetectionAndPassThrough();
  TestDecodeIso88591();
  TestApplyEmbeddedTitleFromMetadata();
  TestUnknownEncodingWithMalformedBytes();
  TestNullAndEmptyInputHandling();
  return 0;
}

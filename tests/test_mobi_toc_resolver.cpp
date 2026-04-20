#include "test_assert.h"

#include "book/book.h"
#include "formats/mobi/mobi_structured_toc_parser.h"
#include "formats/mobi/mobi_record_decode.h"
#include "formats/mobi/mobi_toc_apply.h"
#include "formats/mobi/mobi_toc_finalize.h"
#include "formats/mobi/mobi_toc_resolver.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

class Page {
public:
  std::vector<std::string> lines;
};

namespace {

std::string ToLowerAscii(const std::string &in) {
  std::string out = in;
  for (size_t i = 0; i < out.size(); ++i)
    out[i] = (char)std::tolower((unsigned char)out[i]);
  return out;
}

std::string NormalizeSpaces(const std::string &in) {
  std::string out;
  bool pending_space = false;
  for (size_t i = 0; i < in.size(); ++i) {
    const unsigned char c = (unsigned char)in[i];
    if (std::isspace(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back((char)c);
  }
  return out;
}

std::string NormalizeTitleNeedle(const std::string &text) {
  return ToLowerAscii(NormalizeSpaces(text));
}

size_t CountAsciiWords(const std::string &text) {
  size_t words = 0;
  bool in_word = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = (unsigned char)text[i];
    const bool alpha_num = std::isalnum(c) != 0;
    if (alpha_num && !in_word) {
      ++words;
      in_word = true;
    } else if (!alpha_num) {
      in_word = false;
    }
  }
  return words;
}

bool IsMostlyDigitsOrPunctuation(const std::string &text) {
  size_t significant = 0;
  size_t noisy = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = (unsigned char)text[i];
    if (std::isspace(c))
      continue;
    ++significant;
    if (!std::isalpha(c))
      ++noisy;
  }
  if (significant == 0)
    return true;
  return noisy * 100 >= significant * 90;
}

bool LooksLikeStructuredTitle(const std::string &title) {
  const std::string folded = NormalizeTitleNeedle(title);
  if (folded.find("this page") != std::string::npos)
    return false;
  if (folded.find("contents") != std::string::npos)
    return false;
  if (folded.find("chapter") != std::string::npos)
    return true;
  if (folded.find("part") != std::string::npos)
    return true;
  return CountAsciiWords(folded) >= 1;
}

bool PageHasHeadingNeedle(const std::vector<std::string> &lines,
                          const std::string &needle) {
  for (size_t i = 0; i < lines.size(); ++i) {
    if (NormalizeTitleNeedle(lines[i]).find(needle) != std::string::npos)
      return true;
  }
  return false;
}

void AddChapterAtPageIfUnique(Book *book, u16 page, const std::string &title,
                              u8 level) {
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  for (size_t i = 0; i < chapters.size(); ++i) {
    if (chapters[i].page == page)
      return;
  }
  book->AddChapter(page, title, level);
}

size_t NoPrune(Book *, IStatusReporter *) { return 0; }

bool AlwaysNotNoisy(Book *, mobi_toc_finalize::MobiChapterQualityStats *) {
  return false;
}

bool AlwaysNoisy(Book *, mobi_toc_finalize::MobiChapterQualityStats *stats_out) {
  if (stats_out) {
    stats_out->chapters = 4;
    stats_out->unique_pages = 2;
    stats_out->early_hits = 3;
    stats_out->tiny_titles = 2;
    stats_out->noisy_titles = 2;
    stats_out->structured_titles = 0;
    stats_out->early_window = 4;
  }
  return true;
}

void AddPageWithText(Book *book, const std::vector<std::string> &lines) {
  Page *page = book->AppendPage();
  page->lines = lines;
}

mobi_toc_resolver::InlineTitleCallbacks InlineCallbacks() {
  mobi_toc_resolver::InlineTitleCallbacks cb{};
  cb.looks_like_structured_title = &LooksLikeStructuredTitle;
  cb.fold_latin_for_match = &NormalizeTitleNeedle;
  cb.count_ascii_words = &CountAsciiWords;
  cb.is_mostly_digits_or_punctuation = &IsMostlyDigitsOrPunctuation;
  return cb;
}

mobi_toc_finalize::FinalizeCallbacks FinalizeCallbacks(
    bool noisy = false) {
  mobi_toc_finalize::FinalizeCallbacks cb{};
  cb.normalize_heading_needle = &NormalizeTitleNeedle;
  cb.page_has_heading_needle = &PageHasHeadingNeedle;
  cb.add_chapter_at_page_if_unique = &AddChapterAtPageIfUnique;
  cb.prune_front_matter_toc_cluster = &NoPrune;
  cb.is_heuristic_chapter_set_noisy = noisy ? &AlwaysNoisy : &AlwaysNotNoisy;
  return cb;
}

std::string IdentityDecode(const std::string &in, u32) { return in; }

std::string IdentityNormalize(const std::string &in) { return in; }

bool NeverRejectTitle(const std::string &) { return false; }

mobi_toc_resolver::PrepareCallbacks PrepareCallbacks() {
  mobi_toc_resolver::PrepareCallbacks cb{};
  cb.structured.decode_bytes_to_utf8 = &IdentityDecode;
  cb.structured.normalize_title = &IdentityNormalize;
  cb.structured.reject_title = &NeverRejectTitle;
  cb.inline_title = InlineCallbacks();
  cb.decode_bytes_to_utf8 = &IdentityDecode;
  return cb;
}

void TestInlineTocEntryCreationAndValidation() {
  const std::string markup =
      "<a filepos='100'>Chapter One</a>"
      "<a filepos='250'>Chapter Two</a>"
      "<A FILEPOS='400'>Chapter Three</A>"
      "<a filepos='550'>Chapter Four</a>"
      "<a filepos='650'>Chapter Five</a>"
      "<a filepos='750'>Chapter Six</a>"
      "<a filepos='850'>Chapter Seven</a>"
      "<a filepos='950'>Chapter Eight</a>"
      "<a href='#x'>Missing Position</a>";

  std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry> out;
  const bool ok = mobi_toc_resolver::ParseInlineFileposToc(
      markup, 3000, InlineCallbacks(), &out, nullptr);
  test::ExpectTrue("inline TOC accepted", ok);
  test::ExpectGt("inline TOC count", (int)out.size(), 5);
  test::ExpectEqU("entry0 pos", out[0].pos, 100u);
  test::ExpectStrEq("entry0 title", out[0].title.c_str(), "Chapter One");
  test::ExpectEqU("last entry pos", out[out.size() - 1].pos, 950u);
}

void TestInlineTocEdgeCases() {
  std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry> out;

  test::ExpectFalse(
      "empty markup rejected",
      mobi_toc_resolver::ParseInlineFileposToc("", 2000, InlineCallbacks(), &out,
                                               nullptr));

  test::ExpectFalse(
      "single entry rejected",
      mobi_toc_resolver::ParseInlineFileposToc(
          "<a filepos='77'>Only One</a>", 2000, InlineCallbacks(), &out,
          nullptr));

  test::ExpectFalse(
      "malformed entries rejected",
      mobi_toc_resolver::ParseInlineFileposToc(
          "<a filepos='abc'>Broken</a><a filepos='0'>Zero</a>", 2000,
          InlineCallbacks(), &out, nullptr));
}

void TestStructuredResolverAndInlineFallback() {
  std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry> out;
  bool from_filepos = true;

  const std::string markup =
      "<a filepos='100'>Chapter One</a>"
      "<a filepos='200'>Chapter Two</a>"
      "<a filepos='300'>Chapter Three</a>"
      "<a filepos='400'>Chapter Four</a>"
      "<a filepos='500'>Chapter Five</a>"
      "<a filepos='600'>Chapter Six</a>";

  std::vector<u32> offsets;
  offsets.push_back(0u);
  offsets.push_back(10u);
  offsets.push_back(20u);

  test::ExpectTrue(
      "structured parser path succeeds",
      mobi_toc_resolver::PrepareStructuredToc(
          "", offsets, 5u, 65001u, &markup, 1000u, PrepareCallbacks(), &out,
          &from_filepos, nullptr));
  test::ExpectFalse("structured path not from filepos", from_filepos);
  test::ExpectEq("structured parser produced entries", (int)out.size(), 2);
  test::ExpectStrEq("structured title kept", out[0].title.c_str(),
                    "Structured One");

  out.clear();
  from_filepos = false;
  test::ExpectTrue(
      "inline fallback path succeeds",
      mobi_toc_resolver::PrepareStructuredToc(
          "force-parse-failure", offsets, 5u, 65001u, &markup, 1000u,
          PrepareCallbacks(), &out, &from_filepos, nullptr));
  test::ExpectTrue("inline fallback flagged", from_filepos);
  test::ExpectGt("inline fallback generated entries", (int)out.size(), 3);
}

void TestHtmlPosToPageMapping() {
  std::vector<std::pair<u32, u32>> html_to_text;
  html_to_text.push_back(std::make_pair(0u, 0u));
  html_to_text.push_back(std::make_pair(100u, 1000u));
  html_to_text.push_back(std::make_pair(200u, 2000u));

  std::vector<u32> text_cursor_per_page;
  text_cursor_per_page.push_back(0u);
  text_cursor_per_page.push_back(500u);
  text_cursor_per_page.push_back(1000u);
  text_cursor_per_page.push_back(1500u);
  text_cursor_per_page.push_back(2000u);

  u32 text_pos = 0;
  const int page = mobi_toc_apply::HtmlPosToPage(
      150u, html_to_text, text_cursor_per_page, &text_pos);
  test::ExpectEq("mapped page", page, 3);
  test::ExpectEqU("mapped text pos", text_pos, 1500u);

  test::ExpectEq("invalid map rejected",
                 mobi_toc_apply::HtmlPosToPage(
                     10u, std::vector<std::pair<u32, u32>>(),
                     std::vector<u32>(), nullptr),
                 -1);
}

void TestHeadingHintFallbackGeneration() {
  BookContext ctx;
  Book book(ctx);

  AddPageWithText(&book, std::vector<std::string>(1, "Prologue"));
  AddPageWithText(&book, std::vector<std::string>(1, "Chapter One"));
  AddPageWithText(&book, std::vector<std::string>(1, "Body"));
  AddPageWithText(&book, std::vector<std::string>(1, "Chapter Two"));

  std::vector<mobi_toc_finalize::MobiHeadingHint> hints;
  hints.push_back({"Chapter One", 0});
  hints.push_back({"Chapter Two", 1});

  const size_t mapped =
      mobi_toc_finalize::BuildChaptersFromHints(&book, hints,
                                                FinalizeCallbacks(false));
  test::ExpectEq("heading hint mapped count", (int)mapped, 2);
  const std::vector<ChapterEntry> &chapters = book.GetChapters();
  test::ExpectEq("heading hint chapter count", (int)chapters.size(), 2);
  test::ExpectEq("first heading page", (int)chapters[0].page, 1);
  test::ExpectEq("second heading page", (int)chapters[1].page, 3);
}

void TestTocConfidenceScoringStrongMixedAndLow() {
  BookContext ctx;

  {
    Book strong_book(ctx);
    AddPageWithText(&strong_book, std::vector<std::string>(1, "P0"));
    AddPageWithText(&strong_book, std::vector<std::string>(1, "P1"));
    AddPageWithText(&strong_book, std::vector<std::string>(1, "P2"));
    AddPageWithText(&strong_book, std::vector<std::string>(1, "P3"));

    std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry> structured;
    structured.push_back({"Chapter One", 100u, 0});
    structured.push_back({"Chapter Two", 900u, 0});

    // Provide a valid position map so ShouldApplyStructuredToc returns true.
    std::vector<std::pair<u32, u32>> htm{{0u, 0u}, {1000u, 1000u}};
    std::vector<u32> cursors{0u, 250u, 500u, 750u};
    mobi_toc_finalize::FinalizePreparedToc(
        &strong_book, nullptr, structured, true, true,
        std::vector<mobi_toc_finalize::MobiHeadingHint>(), 1000u,
        htm, cursors,
        FinalizeCallbacks(false), nullptr);

    test::ExpectEq("strong quality", (int)strong_book.GetTocQuality(),
                   (int)TOC_QUALITY_STRONG);
  }

  {
    Book mixed_book(ctx);
    AddPageWithText(&mixed_book, std::vector<std::string>(1, "P0"));
    AddPageWithText(&mixed_book, std::vector<std::string>(1, "P1"));
    AddPageWithText(&mixed_book, std::vector<std::string>(1, "P2"));
    AddPageWithText(&mixed_book, std::vector<std::string>(1, "P3"));
    AddPageWithText(&mixed_book, std::vector<std::string>(1, "P4"));

    std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry> structured;
    structured.push_back({"Chapter One", 100u, 0});
    structured.push_back({"Chapter Two", 900u, 0});
    structured.push_back({"Duplicate Target", 920u, 0});

    std::vector<std::pair<u32, u32>> htm{{0u, 0u}, {1000u, 1000u}};
    std::vector<u32> cursors{0u, 200u, 400u, 600u, 800u};
    mobi_toc_finalize::FinalizePreparedToc(
        &mixed_book, nullptr, structured, true, true,
        std::vector<mobi_toc_finalize::MobiHeadingHint>(), 1000u,
        htm, cursors,
        FinalizeCallbacks(false), nullptr);

    test::ExpectEq("mixed quality", (int)mixed_book.GetTocQuality(),
                   (int)TOC_QUALITY_MIXED);
    test::ExpectEqU("mixed unresolved", mixed_book.GetTocUnresolvedCount(), 1u);
  }

  {
    Book low_book(ctx);
    AddPageWithText(&low_book, std::vector<std::string>(1, "P0"));
    AddPageWithText(&low_book, std::vector<std::string>(1, "P1"));
    low_book.AddChapter(0, "Noisy A", 0);
    low_book.AddChapter(1, "Noisy B", 0);

    mobi_toc_finalize::FinalizePreparedToc(
        &low_book, nullptr,
        std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry>(), false,
        false, std::vector<mobi_toc_finalize::MobiHeadingHint>(), 1000u,
        std::vector<std::pair<u32, u32>>(), std::vector<u32>(),
        FinalizeCallbacks(true), nullptr);

    test::ExpectEq("low/noisy quality becomes unknown",
                   (int)low_book.GetTocQuality(), (int)TOC_QUALITY_UNKNOWN);
    test::ExpectEq("noisy chapters cleared", (int)low_book.GetChapters().size(),
                   0);
  }
}

}

namespace mobi_structured_toc_parser {

bool ParseStructuredToc(const std::string &raw, const std::vector<u32> &, u32,
                        u32, const ParseCallbacks &,
                        std::vector<MobiStructuredTocEntry> *out,
                        IStatusReporter *) {
  if (!out)
    return false;

  static const MobiStructuredTocEntry kEntries[] = {
      {"Structured One", 111u, 0},
      {"Structured Two", 777u, 1},
  };

  out->clear();
  if (!raw.empty())
    return false;
  out->push_back(kEntries[0]);
  out->push_back(kEntries[1]);
  return true;
}

}

namespace file_read_utils {

bool ReadPathToStringLimited(const char *, std::string *, size_t) {
  return false;
}

bool ReadPathToStringLimited(const std::string &, std::string *, size_t) {
  return false;
}

}

namespace mobi_record_scan {

std::uint16_t ReadBE16(const unsigned char *) { return 0; }
std::uint32_t ReadBE32(const unsigned char *) { return 0; }

bool ParseRecordOffsets(const std::string &, std::vector<std::uint32_t> *) {
  return false;
}

unsigned FirstImageProbeLimit(unsigned remaining_records) {
  return remaining_records;
}

unsigned CoverLastResortProbeLimit(unsigned remaining_records) {
  return remaining_records;
}

}

namespace mobi_record_decode {

MobiRecord0Header::MobiRecord0Header()
    : compression(1), text_len(0), text_rec_count(0), encoding(65001u),
      resource_start(0), title_offset(0), title_length(0),
      huffcdic_record_index(0), num_huffcdic_records(0), trailing_flags(0),
      indx_index(0) {}

bool ParseRecord0Header(const uint8_t *, size_t, MobiRecord0Header *) {
  return false;
}

size_t CountBitsSet(uint32_t) { return 0; }

uint32_t GetVarLenFromEnd(const uint8_t *, size_t) { return 0; }

std::string RemoveTrailingEntries(const uint8_t *, size_t, uint32_t) {
  return std::string();
}

bool BuildMergedText(const std::string &, const std::vector<uint32_t> &,
                     const MobiRecord0Header &, std::string *) {
  return false;
}

}

namespace mobi_position_map {

size_t HtmlSampleIntervalForTextBytes(size_t text_bytes) { return text_bytes; }

bool LooksUsableForToc(
    const std::vector<std::pair<uint32_t, uint32_t>> &html_to_text_map,
    const std::vector<uint32_t> &text_cursor_per_page) {
  return html_to_text_map.size() >= 2 && !text_cursor_per_page.empty();
}

void RemapHtmlToTextAfterCleanup(
    const std::string &, const std::string &,
    std::vector<std::pair<uint32_t, uint32_t>> *) {}

}

namespace page_text_extract_utils {

std::vector<std::string> ExtractTextLinesFromPage(Page *page) {
  if (!page)
    return std::vector<std::string>();
  return page->lines;
}

}

Book::Book(const BookContext &ctx) : ctx(ctx) {
  coverPixels = nullptr;
  coverWidth = 0;
  coverHeight = 0;
  coverAttempts = 0;
  metadataIndexTried = false;
  metadataIndexed = false;
  tocResolveTried = false;
  tocResolved = false;
  epub_page_cache_save_pending = false;
  mobi_line_wrap_fix = false;
  parsed_with_mobi_line_wrap_fix = false;
  ClearTocConfidence();
}

Book::~Book() {
  for (size_t i = 0; i < pages.size(); ++i)
    delete pages[i];
  pages.clear();
}

const std::vector<ChapterEntry> &Book::GetChapters() const { return chapters; }

void Book::AddChapter(u16 page, const std::string &title, u8 level) {
  ChapterEntry entry;
  entry.page = page;
  entry.level = level;
  entry.title = title;
  chapters.push_back(entry);
}

void Book::ClearChapters() { chapters.clear(); }

Page *Book::GetPage() {
  if (pages.empty())
    return nullptr;
  return pages[0];
}

Page *Book::GetPage(int i) {
  if (i < 0 || (size_t)i >= pages.size())
    return nullptr;
  return pages[(size_t)i];
}

u16 Book::GetPageCount() { return (u16)pages.size(); }

Page *Book::AppendPage() {
  Page *page = new Page();
  pages.push_back(page);
  return page;
}

int main() {
  TestInlineTocEntryCreationAndValidation();
  TestInlineTocEdgeCases();
  TestStructuredResolverAndInlineFallback();
  TestHtmlPosToPageMapping();
  TestHeadingHintFallbackGeneration();
  TestTocConfidenceScoringStrongMixedAndLow();
  return 0;
}

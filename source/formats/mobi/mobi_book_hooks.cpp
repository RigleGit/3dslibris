#include "formats/mobi/mobi_book_hooks.h"

#include "book/book.h"
#include "book/book_parse_deps.h"
#include "debug_log.h"
#include "formats/common/plain_parser.h"
#include "formats/mobi/mobi_markup_extract.h"
#include "formats/mobi/mobi_safe_markup_extract.h"
#include "formats/mobi/mobi_text_decode.h"
#include "formats/mobi/mobi_toc_finalize.h"
#include "formats/mobi/mobi_toc_prepare.h"
#include "shared/status_reporter.h"

#include <ctype.h>
#include <set>
#include <stdio.h>
#include <vector>

namespace mobi_book_hooks {
namespace {

typedef BookParseDeps BookIoDeps;
using mobi_toc_finalize::MobiHeadingHint;

static std::string DecodeMobiBytesToUtf8(const std::string &in, u32 encoding,
                                         bool *used_utf8_guess,
                                         bool *used_legacy_guess) {
  return mobi_text_decode::DecodeBytesToUtf8(in, encoding, used_utf8_guess,
                                             used_legacy_guess);
}

static size_t PruneMobiFrontMatterTocCluster(Book *book,
                                             IStatusReporter *reporter) {
  if (!book)
    return 0;
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  if (chapters.size() < 20)
    return 0;

  const u16 page_count = book->GetPageCount();
  if (page_count < 220)
    return 0;

  u16 front_page_limit = page_count / 18;
  if (front_page_limit < 28)
    front_page_limit = 28;
  if (front_page_limit > 96)
    front_page_limit = 96;

  size_t prefix_count = 0;
  std::set<u16> prefix_unique_pages;
  for (size_t i = 0; i < chapters.size(); i++) {
    if (chapters[i].page > front_page_limit)
      break;
    prefix_count++;
    prefix_unique_pages.insert(chapters[i].page);
  }

  if (prefix_count < 12)
    return 0;
  if (prefix_count + 8 >= chapters.size())
    return 0;

  const size_t uniq = prefix_unique_pages.size();
  const bool highly_dense = (uniq * 100 < prefix_count * 55);
  const bool large_prefix = (prefix_count * 100 >= chapters.size() * 35);
  if (!highly_dense || !large_prefix)
    return 0;

  std::vector<ChapterEntry> kept;
  kept.reserve(chapters.size() - prefix_count);
  for (size_t i = prefix_count; i < chapters.size(); i++)
    kept.push_back(chapters[i]);

  if (kept.size() < 6)
    return 0;

  book->ClearChapters();
  for (size_t i = 0; i < kept.size(); i++)
    book->AddChapter(kept[i].page, kept[i].title, kept[i].level);

  if (reporter) {
    char msg[224];
    snprintf(
        msg, sizeof(msg),
        "MOBI: TOC front-matter pruned removed=%u remain=%u front_limit<=%u",
        (unsigned)prefix_count, (unsigned)kept.size(),
        (unsigned)front_page_limit);
    DBG_LOG(reporter, msg);
  }
  return prefix_count;
}

static bool LooksLikeStructuredMobiChapterTitle(const std::string &title) {
  std::string compact = plain_parser::CollapseAsciiWhitespace(
      plain_parser::TrimAsciiWhitespace(title));
  if (compact.empty())
    return false;
  bool strong_signal = false;
  return plain_parser::LooksLikePlainChapterHeading(compact, &strong_signal);
}

static bool IsMobiHeuristicChapterSetNoisy(
    Book *book, mobi_toc_finalize::MobiChapterQualityStats *stats_out) {
  if (!book)
    return false;
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  if (chapters.size() < 8)
    return false;

  const u16 page_count = book->GetPageCount();
  if (page_count == 0)
    return false;

  u16 early_window = page_count / 12;
  if (early_window < 12)
    early_window = 12;
  if (early_window > 96)
    early_window = 96;

  size_t early_hits = 0;
  size_t tiny_titles = 0;
  size_t noisy_titles = 0;
  size_t structured_titles = 0;
  std::set<u16> unique_pages;
  for (size_t i = 0; i < chapters.size(); i++) {
    const ChapterEntry &c = chapters[i];
    unique_pages.insert(c.page);
    if (c.page <= early_window)
      early_hits++;
    std::string clean = plain_parser::CollapseAsciiWhitespace(
        plain_parser::TrimAsciiWhitespace(c.title));
    if (clean.size() < 4)
      tiny_titles++;
    if (plain_parser::IsMostlyDigitsOrPunctuation(clean))
      noisy_titles++;
    if (LooksLikeStructuredMobiChapterTitle(clean))
      structured_titles++;
  }

  if (stats_out) {
    stats_out->chapters = chapters.size();
    stats_out->unique_pages = unique_pages.size();
    stats_out->early_hits = early_hits;
    stats_out->tiny_titles = tiny_titles;
    stats_out->noisy_titles = noisy_titles;
    stats_out->structured_titles = structured_titles;
    stats_out->early_window = early_window;
  }

  const size_t n = chapters.size();
  const bool mostly_early = (early_hits * 100 >= n * 65);
  const bool low_spread = (unique_pages.size() * 100 < n * 35);
  const bool mostly_noisy_titles = ((tiny_titles + noisy_titles) * 100 >= n * 55);
  const bool too_unstructured = (structured_titles * 100 < n * 35);
  return mostly_early || low_spread || mostly_noisy_titles || too_unstructured;
}

static std::string DecodeMobiStructuredTocBytes(const std::string &in,
                                                u32 encoding) {
  return DecodeMobiBytesToUtf8(in, encoding, NULL, NULL);
}

static std::string NormalizeMobiStructuredTocTitle(const std::string &in) {
  return plain_parser::CollapseAsciiWhitespace(
      plain_parser::TrimAsciiWhitespace(in));
}

static bool RejectMobiStructuredTocTitle(const std::string &in) {
  return plain_parser::IsMostlyDigitsOrPunctuation(in);
}

static std::string NormalizeHeadingNeedle(const std::string &s) {
  std::string trimmed = plain_parser::TrimAsciiWhitespace(s);
  for (size_t i = 0; i < trimmed.size(); i++) {
    if (trimmed[i] == '|')
      trimmed[i] = ' ';
  }
  return plain_parser::CollapseAsciiWhitespace(
      plain_parser::FoldLatinForMatch(trimmed));
}

static bool PageHasHeadingNeedle(const std::vector<std::string> &lines,
                                 const std::string &needle) {
  if (needle.empty())
    return false;
  const size_t n = lines.size();
  for (size_t i = 0; i < n; i++) {
    std::string norm = NormalizeHeadingNeedle(lines[i]);
    if (norm.empty())
      continue;
    if (norm == needle)
      return true;
    if (norm.size() > needle.size() && norm.find(needle) != std::string::npos)
      return true;
  }
  for (size_t i = 0; i + 1 < n; i++) {
    std::string a = NormalizeHeadingNeedle(lines[i]);
    if (a.empty())
      continue;
    std::string concat = a;
    for (size_t j = i + 1; j < n && concat.size() < needle.size() + 32; j++) {
      std::string b = NormalizeHeadingNeedle(lines[j]);
      if (b.empty())
        continue;
      concat += " " + b;
      if (concat == needle)
        return true;
      if (concat.size() > needle.size() &&
          concat.find(needle) != std::string::npos)
        return true;
    }
  }
  return false;
}

static mobi_toc_finalize::FinalizeCallbacks MakeMobiTocFinalizeCallbacks() {
  mobi_toc_finalize::FinalizeCallbacks callbacks;
  callbacks.normalize_heading_needle = NormalizeHeadingNeedle;
  callbacks.page_has_heading_needle = PageHasHeadingNeedle;
  callbacks.add_chapter_at_page_if_unique = plain_parser::AddChapterAtPageIfUnique;
  callbacks.prune_front_matter_toc_cluster = PruneMobiFrontMatterTocCluster;
  callbacks.is_heuristic_chapter_set_noisy = IsMobiHeuristicChapterSetNoisy;
  return callbacks;
}

static size_t MobiInlineCountWords(const std::string &text) {
  return (size_t)plain_parser::CountAsciiWords(text);
}

static bool MobiInlineMostlyDigitsOrPunct(const std::string &text) {
  return plain_parser::IsMostlyDigitsOrPunctuation(text);
}

static mobi_toc_prepare::StructuredCallbacks MakeMobiStructuredTocCallbacks() {
  mobi_toc_prepare::StructuredCallbacks callbacks;
  callbacks.decode_bytes_to_utf8 = DecodeMobiStructuredTocBytes;
  callbacks.normalize_title = NormalizeMobiStructuredTocTitle;
  callbacks.reject_title = RejectMobiStructuredTocTitle;
  return callbacks;
}

static mobi_toc_prepare::InlineTitleCallbacks MakeMobiInlineTitleCallbacks() {
  mobi_toc_prepare::InlineTitleCallbacks callbacks;
  callbacks.looks_like_structured_title = LooksLikeStructuredMobiChapterTitle;
  callbacks.fold_latin_for_match = plain_parser::FoldLatinForMatch;
  callbacks.count_ascii_words = MobiInlineCountWords;
  callbacks.is_mostly_digits_or_punctuation = MobiInlineMostlyDigitsOrPunct;
  return callbacks;
}

static mobi_markup_extract::ExtractCallbacks MakeMobiMarkupExtractCallbacks() {
  mobi_markup_extract::ExtractCallbacks callbacks;
  callbacks.trim_ascii_whitespace = plain_parser::TrimAsciiWhitespace;
  callbacks.collapse_ascii_whitespace = plain_parser::CollapseAsciiWhitespace;
  callbacks.fold_latin_for_match = plain_parser::FoldLatinForMatch;
  callbacks.looks_like_structured_chapter_title = LooksLikeStructuredMobiChapterTitle;
  callbacks.add_chapter_at_page_if_unique = plain_parser::AddChapterAtPageIfUnique;
  return callbacks;
}

static std::string ExtractMobiMarkupToText(
    Book *book, const BookIoDeps &deps, const std::string &in,
    std::vector<MobiHeadingHint> *heading_hints,
    std::vector<std::pair<u32, u32>> *html_to_text_map) {
  (void)book;
  (void)heading_hints;
  if (html_to_text_map)
    html_to_text_map->clear();
#ifdef DSLIBRIS_DEBUG
  if (deps.reporter) {
    DBG_LOG(deps.reporter, "MOBI: using safe markup extractor");
    DBG_LOG(deps.reporter, "MOBI: safe extractor inline images disabled");
  }
#endif
  return mobi_safe_markup_extract::ExtractToText(in);
}

} // namespace

mobi_parser::Hooks Make() {
  mobi_parser::Hooks hooks;
  hooks.extract_markup_to_text = ExtractMobiMarkupToText;
  hooks.make_structured_toc_callbacks = MakeMobiStructuredTocCallbacks;
  hooks.make_inline_title_callbacks = MakeMobiInlineTitleCallbacks;
  hooks.make_finalize_callbacks = MakeMobiTocFinalizeCallbacks;
  hooks.make_plain_continue_callbacks = plain_parser::MakeContinueCallbacks;
  return hooks;
}

} // namespace mobi_book_hooks

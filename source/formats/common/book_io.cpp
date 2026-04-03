/*
    3dslibris - book_io.cpp
    New module for the 3DS port by Rigle.

    Summary:
    - Input/output and parser dispatch for non-EPUB formats.
    - UTF-8 normalization and encoding repair utilities.
    - TXT/RTF/ODT loading, extraction, and chapter/index helper generation.
*/

#include "book/book.h"
#include "book/book_parse_deps.h"

#include "shared/status_reporter.h"
#include "book/book_xml.h"
#include "formats/common/buffered_status_log.h"
#include "formats/common/book_error.h"
#include "debug_log.h"
#include "formats/epub/epub.h"
#include "formats/common/file_read_utils.h"
#include "book/heading_layout.h"
#include "path_utils.h"
#include "formats/common/plain_text_perf_utils.h"
#include "formats/mobi/mobi_page_cache.h"
#include "formats/mobi/mobi.h"
#include "formats/mobi/mobi_decode_plan.h"
#include "formats/mobi/mobi_heading_markers.h"
#include "formats/mobi/mobi_markup_tag.h"
#include "formats/mobi/mobi_position_map.h"
#include "formats/mobi/mobi_record_decode.h"
#include "formats/mobi/mobi_record_scan.h"
#include "formats/mobi/mobi_structured_toc_parser.h"
#include "formats/mobi/mobi_toc_apply.h"
#include "formats/mobi/mobi_toc_resolver.h"
#include "formats/mobi/mobi_text_cleanup.h"
#include "formats/pdf/pdf.h"
#include "formats/cbz/cbz.h"
#include "book/page.h"
#include "formats/common/page_cache_utils.h"
#include "formats/common/plain_text_stream.h"
#include "formats/common/page_text_extract_utils.h"
#include "formats/common/xml_parse_utils.h"
#include "parse.h"
#include "shared/app_flow_utils.h"
#include "formats/mobi/mobi_deferred_finalize_utils.h"
#include "formats/common/rtf_control_word_utils.h"
#include "formats/common/text_helpers.h"
#include "formats/rtf/rtf_loader.h"
#include "formats/txt/txt_loader.h"
#include "formats/odt/odt_loader.h"
#include "shared/parser_limits.h"
#include "shared/text_layout_utils.h"
#include "string_utils.h"
#include "shared/utf8_utils.h"
#include <algorithm>
#include <ctype.h>
#include <set>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

static std::string DecodeUtf16ToUtf8(const std::string &in);

static const size_t kPlainTextMaxBytes = 12 * 1024 * 1024;
static const size_t kMobiMaxBytes = 64 * 1024 * 1024;
static const u32 kMobiInitialOpenBudgetMs = 320;
static const u16 kMobiInitialOpenPageBudget = 24;

#ifdef DSLIBRIS_DEBUG
static void FlushBufferedStatusLog(
    IStatusReporter *reporter, buffered_status_log::BufferedStatusLog *log) {
  if (!reporter || !log)
    return;
  log->Flush([&](const std::string &chunk) { reporter->PrintStatus(chunk.c_str()); });
}
#endif

typedef BookParseDeps BookIoDeps;
using plain_text_perf_utils::CapturePlainTextPerfBaseline;
using plain_text_perf_utils::FillPlainTextStreamPerf;
using plain_text_perf_utils::LogPlainTextStreamPerf;
using plain_text_perf_utils::PlainTextPerfBaseline;
using plain_text_perf_utils::PlainTextStreamPerf;

static BookIoDeps BuildBookIoDeps(Book *book) { return BuildBookParseDeps(book); }

static void InitParsedataWithBookIoDeps(parsedata_t *parsedata, Book *book,
                                        const BookIoDeps &deps) {
  if (!parsedata)
    return;
  parse_init(parsedata);
  parsedata->reporter = deps.reporter;
  parsedata->ts = deps.ts;
  parsedata->prefs = deps.prefs;
  parsedata->book = book;
}

static bool TryLoadMobiPageCache(Book *book, const char *book_path,
                                 const BookIoDeps &deps) {
  if (!book || !book_path || !deps.reporter || !deps.ts)
    return false;
  Text *ts = deps.ts;
  std::string font = ts->GetFontFile(TEXT_STYLE_REGULAR);
  return mobi_page_cache::TryLoad(
      book, book_path,
      (int)ts->GetPixelSize(), (int)ts->linespacing,
      deps.paragraph_spacing, deps.paragraph_indent,
      deps.orientation, (int)ts->margin.left, (int)ts->margin.right,
      (int)ts->margin.top, (int)ts->margin.bottom,
      font.c_str(),
      book->GetMobiLineWrapFix());
}

static void SaveMobiPageCache(Book *book, const char *book_path,
                              const BookIoDeps &deps,
                              bool line_wrap_fix_enabled) {
  if (!book || !book_path || !deps.reporter || !deps.ts || book->GetPageCount() == 0)
    return;
  Text *ts = deps.ts;
  std::string font = ts->GetFontFile(TEXT_STYLE_REGULAR);
  mobi_page_cache::Save(
      book, book_path,
      (int)ts->GetPixelSize(), (int)ts->linespacing,
      deps.paragraph_spacing, deps.paragraph_indent,
      deps.orientation, (int)ts->margin.left, (int)ts->margin.right,
      (int)ts->margin.top, (int)ts->margin.bottom,
      font.c_str(),
      line_wrap_fix_enabled);
}

static size_t CountNeedleOccurrences(const std::string &haystack,
                                     const char *needle) {
  if (!needle || !*needle)
    return 0;
  size_t count = 0;
  size_t pos = 0;
  const size_t nlen = strlen(needle);
  while (true) {
    size_t hit = haystack.find(needle, pos);
    if (hit == std::string::npos)
      break;
    count++;
    pos = hit + nlen;
  }
  return count;
}

static size_t CountMojibakeMarkers(const std::string &utf8) {
  // Typical markers when valid UTF-8 bytes were decoded as Windows-1252 first
  // (e.g. “ -> â€œ, ’ -> â€™, accented letters -> Ã¡, Ã©, ...).
  size_t score = 0;
  score += CountNeedleOccurrences(utf8, "\xC3\x83");             // Ã
  score += CountNeedleOccurrences(utf8, "\xC3\x82");             // Â
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x82\xAC"); // â€
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x80\x99"); // â€™
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x80\x9C"); // â€œ
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x80\x9D"); // â€
  return score;
}

static bool LooksLikeMojibakeUtf8Text(const std::string &utf8) {
  return CountMojibakeMarkers(utf8) >= 2;
}

static bool IsMobiUtf16Encoding(u32 encoding) {
  return encoding == 1200 || encoding == 65002;
}

static bool IsMobiUtf8Encoding(u32 encoding) { return encoding == 65001; }

static bool IsMobiUnknownEncoding(u32 encoding) {
  return encoding == 0 || encoding == 0xFFFFFFFFu;
}

static std::string DecodeMobiBytesToUtf8(const std::string &in, u32 encoding,
                                         bool *used_utf8_guess,
                                         bool *used_legacy_guess) {
  if (used_utf8_guess)
    *used_utf8_guess = false;
  if (used_legacy_guess)
    *used_legacy_guess = false;

  if (IsMobiUtf16Encoding(encoding))
    return DecodeUtf16ToUtf8(in);

  const bool raw_is_utf8 = LooksLikeValidUtf8Bytes(in);
  const size_t invalid_utf8_leads =
      raw_is_utf8 ? 0 : CountUtf8InvalidLeadBytes(in);
  const bool mostly_utf8 =
      !in.empty() && (invalid_utf8_leads * 1000 <= in.size() * 2); // <=0.2%
  if (IsMobiUtf8Encoding(encoding))
    return raw_is_utf8 ? in : DecodeMostlyUtf8WithCp1252Fallback(in, NULL);

  if (IsMobiUnknownEncoding(encoding)) {
    if (raw_is_utf8) {
      if (used_utf8_guess)
        *used_utf8_guess = true;
      return in;
    }
    if (mostly_utf8) {
      if (used_utf8_guess)
        *used_utf8_guess = true;
      return DecodeMostlyUtf8WithCp1252Fallback(in, NULL);
    }
    if (used_legacy_guess)
      *used_legacy_guess = true;
    return DecodeLegacySingleByteToUtf8(in);
  }

  if (encoding == 1252) {
    std::string legacy_candidate = DecodeLegacySingleByteToUtf8(in);
    if ((raw_is_utf8 || mostly_utf8) &&
        LooksLikeMojibakeUtf8Text(legacy_candidate)) {
      if (raw_is_utf8) {
        if (used_utf8_guess)
          *used_utf8_guess = true;
        return in;
      }
      std::string mixed_utf8 = DecodeMostlyUtf8WithCp1252Fallback(in, NULL);
      const size_t legacy_mojibake = CountMojibakeMarkers(legacy_candidate);
      const size_t mixed_mojibake = CountMojibakeMarkers(mixed_utf8);
      if (legacy_mojibake >= 2 && mixed_mojibake + 1 < legacy_mojibake) {
        if (used_utf8_guess)
          *used_utf8_guess = true;
        return mixed_utf8;
      }
    }
    return legacy_candidate;
  }

  // Unknown-but-declared encoding value: prefer legacy, unless raw UTF-8 is
  // clearly valid and legacy decoding looks mojibake.
  std::string legacy_candidate = DecodeLegacySingleByteToUtf8(in);
  if ((raw_is_utf8 || mostly_utf8) &&
      LooksLikeMojibakeUtf8Text(legacy_candidate)) {
    if (raw_is_utf8) {
      if (used_utf8_guess)
        *used_utf8_guess = true;
      return in;
    }
    std::string mixed_utf8 = DecodeMostlyUtf8WithCp1252Fallback(in, NULL);
    const size_t legacy_mojibake = CountMojibakeMarkers(legacy_candidate);
    const size_t mixed_mojibake = CountMojibakeMarkers(mixed_utf8);
    if (legacy_mojibake >= 2 && mixed_mojibake + 1 < legacy_mojibake) {
      if (used_utf8_guess)
        *used_utf8_guess = true;
      return mixed_utf8;
    }
  }
  return legacy_candidate;
}

static std::string TrimAsciiWhitespace(const std::string &in) {
  size_t start = 0;
  while (start < in.size() && isspace((unsigned char)in[start]))
    start++;
  size_t end = in.size();
  while (end > start && isspace((unsigned char)in[end - 1]))
    end--;
  return in.substr(start, end - start);
}

static std::string CollapseAsciiWhitespace(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool pending_space = false;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (isspace(c)) {
      pending_space = true;
      continue;
    }
    if (pending_space && !out.empty())
      out.push_back(' ');
    pending_space = false;
    out.push_back((char)c);
  }
  return out;
}

static size_t MobiInlineTokenLengthAt(const std::string &text, size_t pos,
                                      u16 *image_id_out = NULL) {
  if (image_id_out)
    *image_id_out = 0;
  if (pos >= text.size())
    return 0;

  size_t image_pos = pos;
  const unsigned char c0 = (unsigned char)text[pos];
  if (c0 == TEXT_IMAGE) {
    if (pos + 2 >= text.size())
      return 0;
  } else if ((c0 == TEXT_IMAGE_CONTEXT_DEFAULT ||
              c0 == TEXT_IMAGE_LEADING_PARAGRAPH ||
              c0 == TEXT_IMAGE_FIGURE_WITH_CAPTION) &&
             pos + 3 < text.size() &&
             (unsigned char)text[pos + 1] == TEXT_IMAGE) {
    image_pos = pos + 1;
  } else {
    return 0;
  }

  if (image_id_out) {
    *image_id_out = (u16)(((u8)text[image_pos + 1] << 8) |
                          (u8)text[image_pos + 2]);
  }
  return image_pos == pos ? 3u : 4u;
}

static void CollectMobiInlineImageTokenIds(const std::string &text,
                                           std::vector<u16> *ids) {
  if (!ids)
    return;
  ids->clear();
  for (size_t i = 0; i < text.size();) {
    u16 image_id = 0;
    const size_t token_len = MobiInlineTokenLengthAt(text, i, &image_id);
    if (token_len == 0) {
      i++;
      continue;
    }
    ids->push_back(image_id);
    i += token_len;
  }
}

static bool HasMobiInlineImageTokens(const std::string &text) {
  for (size_t i = 0; i < text.size();) {
    const size_t token_len = MobiInlineTokenLengthAt(text, i, NULL);
    if (token_len != 0)
      return true;
    i++;
  }
  return false;
}

static std::string FoldLatinForMatch(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x80) {
      out.push_back((char)tolower(c));
      continue;
    }
    if (c == 0xC3 && i + 1 < in.size()) {
      unsigned char c2 = (unsigned char)in[i + 1];
      switch (c2) {
      case 0x81:
      case 0xA1:
        out.push_back('a');
        i++;
        continue;
      case 0x89:
      case 0xA9:
        out.push_back('e');
        i++;
        continue;
      case 0x8D:
      case 0xAD:
        out.push_back('i');
        i++;
        continue;
      case 0x93:
      case 0xB3:
        out.push_back('o');
        i++;
        continue;
      case 0x9A:
      case 0xBA:
      case 0x9C:
      case 0xBC:
        out.push_back('u');
        i++;
        continue;
      case 0x91:
      case 0xB1:
        out.push_back('n');
        i++;
        continue;
      case 0x87:
      case 0xA7:
        out.push_back('c');
        i++;
        continue;
      default:
        break;
      }
    }
  }
  return out;
}

static bool StartsWithChapterPrefix(const std::string &folded) {
  static const char *kPrefixes[] = {
      "chapter", "capitulo", "parte", "part",
      "seccion", "section",  "book",  "libro",
  };
  for (size_t i = 0; i < sizeof(kPrefixes) / sizeof(kPrefixes[0]); i++) {
    const char *prefix = kPrefixes[i];
    size_t len = strlen(prefix);
    if (folded.size() < len || folded.compare(0, len, prefix) != 0)
      continue;
    if (folded.size() == len)
      return true;
    char next = folded[len];
    if (next == ' ' || next == ':' || next == '-' || next == '.' ||
        next == ')' || isdigit((unsigned char)next))
      return true;
  }
  return false;
}

static bool IsRomanHeadingToken(const std::string &token) {
  if (token.empty() || token.size() > 8)
    return false;
  for (size_t i = 0; i < token.size(); i++) {
    char c = token[i];
    if (c != 'i' && c != 'v' && c != 'x' && c != 'l' && c != 'c' && c != 'd' &&
        c != 'm')
      return false;
  }
  return true;
}

static std::string TrimRomanTokenSuffix(const std::string &token) {
  size_t end = token.size();
  while (end > 0) {
    char c = token[end - 1];
    if (c == '.' || c == ')' || c == ':' || c == '-')
      end--;
    else
      break;
  }
  return token.substr(0, end);
}

static bool IsUpperAsciiRomanToken(const std::string &token) {
  std::string clean = TrimRomanTokenSuffix(token);
  if (clean.empty() || clean.size() > 8)
    return false;
  for (size_t i = 0; i < clean.size(); i++) {
    char c = clean[i];
    if (c != 'I' && c != 'V' && c != 'X' && c != 'L' && c != 'C' && c != 'D' &&
        c != 'M')
      return false;
  }
  return true;
}

static bool LooksLikeTocLeaderLine(const std::string &folded) {
  if (folded.find("...") == std::string::npos &&
      folded.find(" . .") == std::string::npos)
    return false;

  int i = (int)folded.size() - 1;
  while (i >= 0 && isspace((unsigned char)folded[(size_t)i]))
    i--;
  int digits = 0;
  while (i >= 0 && isdigit((unsigned char)folded[(size_t)i])) {
    digits++;
    i--;
  }
  return digits > 0;
}

static int CountAsciiWords(const std::string &line) {
  int words = 0;
  bool in_word = false;
  for (size_t i = 0; i < line.size(); i++) {
    unsigned char c = (unsigned char)line[i];
    if (isspace(c)) {
      in_word = false;
      continue;
    }
    if (!in_word) {
      words++;
      in_word = true;
    }
  }
  return words;
}

static bool EndsWithStandaloneDigits(const std::string &folded,
                                     int *digits_out) {
  if (digits_out)
    *digits_out = 0;
  if (folded.empty())
    return false;

  int i = (int)folded.size() - 1;
  while (i >= 0 && isspace((unsigned char)folded[(size_t)i]))
    i--;
  if (i < 0)
    return false;

  int digits = 0;
  while (i >= 0 && isdigit((unsigned char)folded[(size_t)i])) {
    digits++;
    i--;
  }
  if (digits == 0)
    return false;
  if (digits_out)
    *digits_out = digits;

  if (i < 0)
    return false;
  return isspace((unsigned char)folded[(size_t)i]) != 0;
}

static bool LooksLikeFalsePositiveHeading(const std::string &compact,
                                          const std::string &folded,
                                          bool strong_signal) {
  if (compact.empty())
    return true;

  if (folded == "contents" || folded == "table of contents" ||
      folded == "indice" || folded == "indice de contenido" ||
      folded == "contenido")
    return true;

  if (LooksLikeTocLeaderLine(folded))
    return true;

  if (folded.find('?') != std::string::npos ||
      folded.find('!') != std::string::npos ||
      folded.find(';') != std::string::npos)
    return true;

  int trailing_digits = 0;
  if (!strong_signal && EndsWithStandaloneDigits(folded, &trailing_digits) &&
      trailing_digits >= 1 && trailing_digits <= 4 &&
      CountAsciiWords(compact) >= 3) {
    // Common "title .... page" / "title page" in printed TOCs.
    return true;
  }

  return false;
}

static bool LooksLikePlainChapterHeading(const std::string &line,
                                         bool *strong_signal_out) {
  if (strong_signal_out)
    *strong_signal_out = false;

  std::string trimmed = TrimAsciiWhitespace(line);
  if (trimmed.size() < 4 || trimmed.size() > 120)
    return false;

  std::string compact = CollapseAsciiWhitespace(trimmed);
  std::string folded = FoldLatinForMatch(compact);
  if (folded.size() < 4)
    return false;
  if (StartsWithChapterPrefix(folded)) {
    if (strong_signal_out)
      *strong_signal_out = true;
    return true;
  }

  size_t split = folded.find(' ');
  std::string first =
      split == std::string::npos ? folded : folded.substr(0, split);
  std::string rest = split == std::string::npos ? "" : folded.substr(split + 1);

  size_t split_compact = compact.find(' ');
  std::string first_compact = split_compact == std::string::npos
                                  ? compact
                                  : compact.substr(0, split_compact);
  std::string rest_compact = split_compact == std::string::npos
                                 ? ""
                                 : compact.substr(split_compact + 1);

  if (IsRomanHeadingToken(first) && IsUpperAsciiRomanToken(first_compact) &&
      !TrimAsciiWhitespace(rest).empty() &&
      !TrimAsciiWhitespace(rest_compact).empty()) {
    if (strong_signal_out)
      *strong_signal_out = true;
    return true;
  }

  size_t p = 0;
  while (p < folded.size() && isdigit((unsigned char)folded[p]))
    p++;
  if (p > 0 && p < folded.size() &&
      (folded[p] == '.' || folded[p] == ')' || folded[p] == '-') &&
      p + 1 < folded.size() && folded[p + 1] == ' ') {
    if (strong_signal_out)
      *strong_signal_out = true;
    return true;
  }

  return false;
}

static bool IsBlankLine(const std::string &line) {
  return TrimAsciiWhitespace(line).empty();
}

static bool ShouldAcceptHeuristicHeading(const std::string &line,
                                         bool prev_blank, bool next_blank,
                                         bool prev_candidate,
                                         bool next_candidate,
                                         bool strong_signal) {
  std::string compact = CollapseAsciiWhitespace(TrimAsciiWhitespace(line));
  std::string folded = FoldLatinForMatch(compact);
  if (LooksLikeFalsePositiveHeading(compact, folded, strong_signal))
    return false;

  if (prev_blank || next_blank)
    return true;

  // Dense clusters of candidate lines are usually in-book printed TOCs.
  if (prev_candidate || next_candidate)
    return false;

  // Allow compact chapter-like headings even without blank separators.
  return strong_signal && CountAsciiWords(compact) <= 8;
}

static void AddChapterAtPageIfUnique(Book *book, u16 page,
                                     const std::string &title, u8 level) {
  if (!book)
    return;
  std::string clean = CollapseAsciiWhitespace(TrimAsciiWhitespace(title));
  if (clean.empty())
    return;
  if (book->GetChapters().size() >= 512)
    return;

  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  if (!chapters.empty()) {
    const ChapterEntry &last = chapters.back();
    if (last.page == page && last.title == clean && last.level == level)
      return;
  }
  book->AddChapter(page, clean, level);
}

static bool IsMostlyDigitsOrPunctuation(const std::string &s) {
  std::string t = CollapseAsciiWhitespace(TrimAsciiWhitespace(s));
  if (t.empty())
    return true;
  int alpha = 0;
  int total = 0;
  for (size_t i = 0; i < t.size(); i++) {
    unsigned char c = (unsigned char)t[i];
    if (c < 0x80) {
      if (isalnum(c))
        total++;
      if (isalpha(c))
        alpha++;
      continue;
    }
    // Non-ASCII UTF-8 lead byte -> treat as alpha-like token content.
    if ((c & 0xC0) == 0xC0) {
      alpha++;
      total++;
    }
  }
  if (total == 0)
    return true;
  return alpha <= 1;
}

struct MobiChapterQualityStats {
  size_t chapters;
  size_t unique_pages;
  size_t early_hits;
  size_t tiny_titles;
  size_t noisy_titles;
  size_t structured_titles;
  u16 early_window;
};

static size_t PruneMobiFrontMatterTocCluster(Book *book, IStatusReporter *reporter) {
  if (!book)
    return 0;
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  if (chapters.size() < 20)
    return 0;

  const u16 page_count = book->GetPageCount();
  if (page_count < 220)
    return 0;

  // In large MOBI books, printed "Table of Contents" pages can be mapped as
  // dozens of pseudo-chapters concentrated near the beginning.
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
    return 0; // Avoid pruning when almost everything is in front matter.

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
  std::string compact = CollapseAsciiWhitespace(TrimAsciiWhitespace(title));
  if (compact.empty())
    return false;
  std::string folded = FoldLatinForMatch(compact);
  if (StartsWithChapterPrefix(folded))
    return true;

  size_t sp = compact.find(' ');
  std::string first =
      (sp == std::string::npos) ? compact : compact.substr(0, sp);
  std::string first_folded =
      (sp == std::string::npos) ? folded : folded.substr(0, folded.find(' '));
  if (IsUpperAsciiRomanToken(first) && IsRomanHeadingToken(first_folded))
    return true;

  size_t p = 0;
  while (p < folded.size() && isdigit((unsigned char)folded[p]))
    p++;
  if (p > 0 && p < folded.size() &&
      (folded[p] == '.' || folded[p] == ')' || folded[p] == '-') &&
      p + 1 < folded.size() && folded[p + 1] == ' ')
    return true;

  return false;
}

static bool IsMobiHeuristicChapterSetNoisy(Book *book,
                                           MobiChapterQualityStats *stats_out) {
  if (!book)
    return false;
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  if (chapters.size() < 8)
    return false;

  const u16 page_count = book->GetPageCount();
  if (page_count == 0)
    return false;

  u16 early_window = page_count / 12; // ~first 8%
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
    std::string clean = CollapseAsciiWhitespace(TrimAsciiWhitespace(c.title));
    if (clean.size() < 4)
      tiny_titles++;
    if (IsMostlyDigitsOrPunctuation(clean))
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
  const bool mostly_noisy_titles =
      ((tiny_titles + noisy_titles) * 100 >= n * 55);
  const bool too_unstructured = (structured_titles * 100 < n * 35);
  return mostly_early || low_spread || mostly_noisy_titles || too_unstructured;
}

static void AddChapterIfUnique(Book *book, const std::string &title, u8 level) {
  if (!book)
    return;
  AddChapterAtPageIfUnique(book, book->GetPageCount(), title, level);
}

static void SetNonEpubTocConfidence(Book *book, bool strong) {
  if (!book)
    return;
  size_t n = book->GetChapters().size();
  if (n == 0) {
    book->ClearTocConfidence();
    return;
  }
  u16 count = (n > 65535) ? 65535 : (u16)n;
  if (strong)
    book->SetTocConfidence(TOC_QUALITY_STRONG, count, 0, 0);
  else
    book->SetTocConfidence(TOC_QUALITY_HEURISTIC, 0, count, 0);
}

static void FinalizePlainPage(parsedata_t *p) {
  if (!p || !p->book)
    return;
  if (p->buflen > 0 || p->book->GetPageCount() == 0) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
  }
}

typedef plain_text_stream::State PlainTextStreamState;

static bool InitPlainTextStreamState(Book *book, const std::string &text_utf8,
                                     const BookIoDeps &deps,
                                     bool detect_heuristic_headings,
                                     PlainTextStreamState *out) {
  if (!book || !deps.ts || !out)
    return false;

  parsedata_t base{};
  InitParsedataWithBookIoDeps(&base, book, deps);
  plain_text_stream::InitState(out, base, text_utf8, detect_heuristic_headings);
  return true;
}

static void PlainAdvanceScreen(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;

  Text *ts = p->ts;
  if (p->screen == 1) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    parse_reset_page_buffer(p);
    if (p->italic)
      parse_append_page_byte(p, TEXT_ITALIC_ON);
    if (p->bold)
      parse_append_page_byte(p, TEXT_BOLD_ON);
    p->screen = 0;
  } else {
    p->screen = 1;
  }

  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + ts->GetHeight();
  p->linebegan = false;
}

static bool PlainForceAdvancePageForBufferLimit(parsedata_t *p, void *ctx) {
  (void)ctx;
  if (!p || !p->ts || !p->book)
    return false;

  Text *ts = p->ts;
  Page *page = p->book->AppendPage();
  page->SetBuffer(p->buf, p->buflen);
  parse_reset_page_buffer(p);
  if (p->italic)
    parse_append_page_byte(p, TEXT_ITALIC_ON);
  if (p->bold)
    parse_append_page_byte(p, TEXT_BOLD_ON);
  p->screen = 0;
  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + ts->GetHeight();
  p->linebegan = false;
  return true;
}

static void PlainLinefeed(parsedata_t *p) {
  if (!p || !p->ts)
    return;
  parse_append_page_byte(p, '\n');
  p->pen.x = p->ts->margin.left;
  p->pen.y += p->ts->GetHeight() + p->ts->linespacing;
  p->linebegan = false;
}

static bool ApplyPlainHeadingKeepWithNext(parsedata_t *p, int heading_level) {
  if (!p || !p->ts)
    return false;
  if (heading_level < 1 || heading_level > 3)
    return false;

  heading_layout::KeepWithNextRequest req{};
  req.pen_y = p->pen.y;
  req.screen_height = (p->screen == 1) ? 320 : 400;
  req.bottom_margin =
      (p->screen == 1) ? MIN(p->ts->margin.bottom, 16) : p->ts->margin.bottom;
  req.line_height = p->ts->GetHeight();
  req.linespacing = p->ts->linespacing;
  req.heading_level = heading_level;
  if (heading_layout::ShouldAdvanceHeadingForKeepWithNext(req)) {
    PlainAdvanceScreen(p);
    return true;
  }
  return false;
}

static void AppendInlineImageToPlainParsedData(parsedata_t *p, u16 image_id,
                                               InlineImageContext image_context) {
  if (!p || !p->ts || !p->book)
    return;

  const size_t token_len = 4;
  if (p->buflen > 0 && ((size_t)p->buflen + token_len) > PAGEBUFSIZE)
    PlainForceAdvancePageForBufferLimit(p, NULL);

  Text *ts = p->ts;
  InlineImageLayoutPlan image_plan{};
  p->book->PlanInlineImageLayout(ts, image_id, p->screen, p->pen.x, p->pen.y,
                                 p->linebegan, image_context,
                                 &image_plan);

  if (image_plan.advance_before)
    PlainAdvanceScreen(p);
  if (image_plan.line_break_before && p->linebegan)
    PlainLinefeed(p);

  if (image_context == INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH)
    parse_append_page_byte(p, TEXT_IMAGE_LEADING_PARAGRAPH);
  else if (image_context == INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION)
    parse_append_page_byte(p, TEXT_IMAGE_FIGURE_WITH_CAPTION);
  else
    parse_append_page_byte(p, TEXT_IMAGE_CONTEXT_DEFAULT);
  parse_append_page_byte(p, TEXT_IMAGE);
  parse_append_page_byte(p, (u8)((image_id >> 8) & 0xFF));
  parse_append_page_byte(p, (u8)(image_id & 0xFF));
  p->perf_inline_images++;

  switch (image_plan.mode) {
  case INLINE_IMAGE_LAYOUT_INLINE:
    p->pen.x += image_plan.draw_width + ts->GetAdvance(' ');
    p->linebegan = true;
    break;
  case INLINE_IMAGE_LAYOUT_BAND:
    p->pen.x = ts->margin.left;
    p->pen.y += image_plan.vertical_space_after_draw;
    p->linebegan = false;
    break;
  case INLINE_IMAGE_LAYOUT_PAGE:
  default:
    if (p->screen == 1) {
      PlainAdvanceScreen(p);
    } else {
      p->screen = 1;
      p->pen.x = ts->margin.left;
      p->pen.y = ts->margin.top + ts->GetHeight();
      p->linebegan = false;
    }
    break;
  }
}

// text_cursor_per_page: if non-null, receives one entry per page created
// during this call — the text byte offset at the start of that page.
// Together with html_to_text_map (built in ExtractMobiMarkupToText), this
// allows MobiHtmlPosToPage() to convert MOBI TOC byte positions into accurate
// page numbers without the lossy linear ratio that was used before.
static bool ContinuePlainTextStreamState(PlainTextStreamState *state,
                                         const std::string &text_utf8,
                                         u32 budget_ms, u16 page_budget,
                                         u16 min_pages_before_stop,
                                         std::vector<u32> *text_cursor_per_page,
                                         PlainTextStreamPerf *perf_out) {
  plain_text_stream::ContinueCallbacks callbacks;
  callbacks.is_blank_line = IsBlankLine;
  callbacks.looks_like_plain_chapter_heading = LooksLikePlainChapterHeading;
  callbacks.should_accept_heuristic_heading = ShouldAcceptHeuristicHeading;
  callbacks.add_chapter_if_unique = AddChapterIfUnique;
  callbacks.apply_heading_keep_with_next = ApplyPlainHeadingKeepWithNext;
  callbacks.append_inline_image_to_parsedata = AppendInlineImageToPlainParsedData;
  callbacks.finalize_plain_page = FinalizePlainPage;
  callbacks.set_non_epub_toc_confidence = SetNonEpubTocConfidence;
  return plain_text_stream::ContinueState(
      state, text_utf8, budget_ms, page_budget, min_pages_before_stop,
      text_cursor_per_page, perf_out, callbacks);
}

static u8 ParsePlainTextBuffer(Book *book, const std::string &text_utf8,
                               bool detect_heuristic_headings = true) {
  if (!book)
    return 1;
  const BookIoDeps deps = BuildBookIoDeps(book);
  book->ClearChapters();
  book->ClearTocConfidence();

  PlainTextStreamState state;
  if (!InitPlainTextStreamState(book, text_utf8, deps,
                                detect_heuristic_headings, &state))
    return 1;
  PlainTextStreamPerf perf;
  ContinuePlainTextStreamState(&state, text_utf8, 0, 0, 0, nullptr, &perf);
#ifdef DSLIBRIS_DEBUG
  LogPlainTextStreamPerf(deps.reporter, "PLAIN", perf, state.completed);
#endif
  return 0;
}

static void BuildFb2FallbackChapters(Book *book) {
  if (!book)
    return;
  if (!book->GetChapters().empty())
    return;
  if (book->GetPageCount() == 0)
    return;

  std::vector<std::string> lines;
  std::vector<u16> line_pages;
  for (u16 page = 0; page < book->GetPageCount(); page++) {
    const std::vector<std::string> page_lines =
        page_text_extract_utils::ExtractTextLinesFromPage(book->GetPage(page));
    for (size_t i = 0; i < page_lines.size(); i++) {
      lines.push_back(page_lines[i]);
      line_pages.push_back(page);
    }
  }

  if (lines.empty())
    return;

  bool prev_blank = true;
  bool prev_candidate = false;
  for (size_t i = 0; i < lines.size(); i++) {
    bool curr_blank = IsBlankLine(lines[i]);
    bool next_blank = (i + 1 >= lines.size()) || IsBlankLine(lines[i + 1]);

    bool curr_strong = false;
    bool curr_candidate = LooksLikePlainChapterHeading(lines[i], &curr_strong);
    bool next_strong = false;
    bool next_candidate =
        (i + 1 < lines.size()) &&
        LooksLikePlainChapterHeading(lines[i + 1], &next_strong);

    if (curr_candidate && ShouldAcceptHeuristicHeading(
                              lines[i], prev_blank, next_blank, prev_candidate,
                              next_candidate, curr_strong)) {
      AddChapterAtPageIfUnique(book, line_pages[i], lines[i], 0);
    }

    prev_blank = curr_blank;
    prev_candidate = curr_candidate;
  }
}

static u8 ParseTxtFile(Book *book, const char *path) {
  std::string text;
  if (!txt_loader::ReadAndNormalize(path, &text))
    return BOOK_ERR_CORRUPT;
  return ParsePlainTextBuffer(book, text);
}

static u8 ParseRtfFile(Book *book, const char *path) {
  std::string text;
  if (!rtf_loader::ReadAndDecode(path, &text))
    return BOOK_ERR_CORRUPT;
  return ParsePlainTextBuffer(book, text);
}

static const u32 kMobiNullIndex = mobi_structured_toc_parser::kMobiNullIndex;
typedef mobi_structured_toc_parser::MobiStructuredTocEntry MobiStructuredTocEntry;

static std::string DecodeMobiStructuredTocBytes(const std::string &in,
                                                u32 encoding) {
  return DecodeMobiBytesToUtf8(in, encoding, NULL, NULL);
}

static std::string NormalizeMobiStructuredTocTitle(const std::string &in) {
  return CollapseAsciiWhitespace(TrimAsciiWhitespace(in));
}

static bool RejectMobiStructuredTocTitle(const std::string &in) {
  return IsMostlyDigitsOrPunctuation(in);
}

std::string DecodeUtf16ToUtf8(const std::string &in) {
  std::string out;
  if (in.size() < 2)
    return out;

  bool little_endian = true;
  size_t i = 0;
  if ((unsigned char)in[0] == 0xFE && (unsigned char)in[1] == 0xFF) {
    little_endian = false;
    i = 2;
  } else if ((unsigned char)in[0] == 0xFF && (unsigned char)in[1] == 0xFE) {
    little_endian = true;
    i = 2;
  }

  while (i + 1 < in.size()) {
    u16 w1 =
        little_endian
            ? ((u16)(unsigned char)in[i] | ((u16)(unsigned char)in[i + 1] << 8))
            : (((u16)(unsigned char)in[i] << 8) |
               (u16)(unsigned char)in[i + 1]);
    i += 2;
    u32 cp = w1;
    if (w1 >= 0xD800 && w1 <= 0xDBFF && i + 1 < in.size()) {
      u16 w2 = little_endian ? ((u16)(unsigned char)in[i] |
                                ((u16)(unsigned char)in[i + 1] << 8))
                             : (((u16)(unsigned char)in[i] << 8) |
                                (u16)(unsigned char)in[i + 1]);
      if (w2 >= 0xDC00 && w2 <= 0xDFFF) {
        cp = 0x10000u + (((u32)(w1 - 0xD800) << 10) | (u32)(w2 - 0xDC00));
        i += 2;
      }
    }
    AppendUtf8Codepoint(&out, cp);
  }
  return out;
}

static void AppendParagraphBreak(std::string *out) {
  if (!out)
    return;
  while (!out->empty() &&
         (out->back() == ' ' || out->back() == '\t' || out->back() == '\r'))
    out->pop_back();
  if (out->empty()) {
    out->append("\n\n");
    return;
  }
  if (out->size() >= 2 && (*out)[out->size() - 1] == '\n' &&
      (*out)[out->size() - 2] == '\n')
    return;
  if (out->back() == '\n')
    out->push_back('\n');
  else
    out->append("\n\n");
}

static void AppendSingleSpace(std::string *out) {
  if (!out)
    return;
  if (out->empty())
    return;
  char tail = out->back();
  if (tail != ' ' && tail != '\n' && tail != '\t' && tail != '\r')
    out->push_back(' ');
}

struct MobiHeadingHint {
  std::string title;
  u8 level;
};

struct MobiDeferredState {
  PlainTextStreamState stream;
  std::string source_path;
  std::string markup_utf8;
  std::string text_utf8;
  std::vector<MobiHeadingHint> heading_hints;
  std::vector<MobiStructuredTocEntry> structured_toc;
  // OPT-A mapping tables — built during fresh parse, empty on cache loads.
  std::vector<std::pair<u32, u32>> html_to_text_map;
  std::vector<u32> text_cursor_per_page;
  bool have_structured_toc;
  bool structured_from_filepos;
  bool toc_metadata_ready;
  bool structured_toc_loaded;
  bool toc_applied;
  bool cache_saved;
  bool used_utf8_guess;
  bool used_legacy_guess;
  bool line_wrap_fix_applied;
  bool finalized;
  u32 text_len_for_pos;
  u64 t_parse_begin;
  u64 t_after_read;
  u64 t_after_decompress;
  u64 t_after_decode;
  u64 t_after_markup_scan;
  u64 t_after_cleanup;
  u64 t_after_initial_pages;
  u64 t_after_markup;
  u64 t_after_pages;
  u64 t_after_toc;
};

static std::unordered_map<const Book *, MobiDeferredState>
    g_mobi_deferred_states;

static std::string NormalizeHeadingNeedle(const std::string &s) {
  std::string trimmed = TrimAsciiWhitespace(s);
  for (size_t i = 0; i < trimmed.size(); i++) {
    if (trimmed[i] == '|')
      trimmed[i] = ' ';
  }
  return CollapseAsciiWhitespace(FoldLatinForMatch(trimmed));
}

static bool PageHasHeadingNeedle(const std::vector<std::string> &lines,
                                 const std::string &needle) {
  if (needle.empty())
    return false;
  // Search all lines on the page.  Pages are short (3DS screen ≈ 30-40 lines)
  // and decorative whitespace (pagebreaks, <br>) generates many blank entries
  // that previously exhausted the old cap of 18 before reaching real content.
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
  // Multi-line headings: try concatenating consecutive non-empty lines.
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

static size_t
BuildMobiChaptersFromHints(Book *book,
                           const std::vector<MobiHeadingHint> &hints) {
  if (!book || hints.empty() || book->GetPageCount() == 0)
    return 0;

  std::vector<std::vector<std::string>> page_lines;
  page_lines.resize(book->GetPageCount());
  for (u16 p = 0; p < book->GetPageCount(); p++)
    page_lines[p] =
        page_text_extract_utils::ExtractTextLinesFromPage(book->GetPage(p));

  size_t mapped = 0;
  u16 scan_start = 0;
  const u16 page_count = book->GetPageCount();
  for (size_t i = 0; i < hints.size(); i++) {
    std::string needle = NormalizeHeadingNeedle(hints[i].title);
    if (needle.size() < 3)
      continue;

    int best = -1;
    for (u16 p = scan_start; p < page_count; p++) {
      if (PageHasHeadingNeedle(page_lines[p], needle)) {
        best = (int)p;
        break;
      }
    }
    if (best < 0 && scan_start > 0) {
      for (u16 p = 0; p < scan_start; p++) {
        if (PageHasHeadingNeedle(page_lines[p], needle)) {
          best = (int)p;
          break;
        }
      }
    }
    if (best < 0)
      continue;

    AddChapterAtPageIfUnique(book, (u16)best, hints[i].title, hints[i].level);
    mapped++;
    scan_start = (u16)best;
  }
  return mapped;
}

static std::string MobiTocApplyNormalizeNeedle(const std::string &text) {
  return NormalizeHeadingNeedle(text);
}

static bool MobiTocApplyPageHasNeedle(const std::vector<std::string> &lines,
                                      const std::string &needle) {
  return PageHasHeadingNeedle(lines, needle);
}

static void MobiTocApplyAddChapterAtPageIfUnique(Book *book, u16 page,
                                                 const std::string &title,
                                                 u8 level) {
  AddChapterAtPageIfUnique(book, page, title, level);
}

struct MobiTocFinalizeResult {
  size_t mapped_chapters;
  size_t structured_entries;
  size_t structured_direct;
  bool structured_from_filepos;
};

static void FinalizeMobiPreparedToc(
    Book *book, IStatusReporter *reporter,
    const std::vector<MobiStructuredTocEntry> &structured_toc,
    bool have_structured_toc, bool structured_from_filepos,
    const std::vector<MobiHeadingHint> &heading_hints, u32 text_len_for_pos,
    const std::vector<std::pair<u32, u32>> &html_to_text_map,
    const std::vector<u32> &text_cursor_per_page,
    MobiTocFinalizeResult *out) {
  if (!book)
    return;

  if (out) {
    out->mapped_chapters = 0;
    out->structured_entries = structured_toc.size();
    out->structured_direct = 0;
    out->structured_from_filepos = structured_from_filepos;
  }

  size_t mapped_chapters = 0;
  size_t mapped_structured = 0;
  size_t structured_direct = 0;
  bool structured_used = false;

  if (have_structured_toc) {
    const std::vector<ChapterEntry> fallback = book->GetChapters();
    book->ClearChapters();
    mobi_toc_apply::BuildCallbacks toc_callbacks;
    toc_callbacks.normalize_heading_needle = MobiTocApplyNormalizeNeedle;
    toc_callbacks.page_has_heading_needle = MobiTocApplyPageHasNeedle;
    toc_callbacks.add_chapter_at_page_if_unique =
        MobiTocApplyAddChapterAtPageIfUnique;
    mapped_structured = mobi_toc_apply::BuildChaptersFromStructuredToc(
        book, structured_toc, text_len_for_pos, &structured_direct,
        !structured_from_filepos, html_to_text_map, text_cursor_per_page,
        toc_callbacks, reporter);
    if (mapped_structured >= 2) {
      structured_used = true;
      PruneMobiFrontMatterTocCluster(book, reporter);
      mapped_structured = book->GetChapters().size();

      u16 direct = (structured_direct > 65535) ? 65535 : (u16)structured_direct;
      u16 unresolved = 0;
      if (structured_toc.size() > mapped_structured) {
        size_t miss = structured_toc.size() - mapped_structured;
        unresolved = (miss > 65535) ? 65535 : (u16)miss;
      }
      TocQuality quality = TOC_QUALITY_MIXED;
      if (unresolved == 0 && mapped_structured > 0 &&
          structured_direct * 100 >= mapped_structured * 85) {
        quality = TOC_QUALITY_STRONG;
      }
      book->SetTocConfidence(quality, direct, 0, unresolved);
      mapped_chapters = mapped_structured;
      if (reporter && structured_from_filepos) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "MOBI: filepos TOC mapped=%u direct=%u unresolved=%u",
                 (unsigned)mapped_structured, (unsigned)direct,
                 (unsigned)unresolved);
        DBG_LOG(reporter, msg);
      }
    } else {
      book->ClearChapters();
      for (size_t i = 0; i < fallback.size(); i++) {
        book->AddChapter(fallback[i].page, fallback[i].title,
                         fallback[i].level);
      }
    }
  }

  size_t mapped_hints = 0;
  if (!structured_used && !heading_hints.empty()) {
    const std::vector<ChapterEntry> fallback = book->GetChapters();
    book->ClearChapters();
    mapped_hints = BuildMobiChaptersFromHints(book, heading_hints);
    if (mapped_hints < 2) {
      book->ClearChapters();
      for (size_t i = 0; i < fallback.size(); i++) {
        book->AddChapter(fallback[i].page, fallback[i].title,
                         fallback[i].level);
      }
    } else {
      mapped_chapters = mapped_hints;
    }
  }

  if (!structured_used && mapped_hints >= 2) {
    u16 mapped = (mapped_hints > 65535) ? 65535 : (u16)mapped_hints;
    u16 unresolved = 0;
    if (heading_hints.size() > mapped_hints) {
      size_t miss = heading_hints.size() - mapped_hints;
      unresolved = (miss > 65535) ? 65535 : (u16)miss;
    }
    TocQuality quality =
        (unresolved == 0) ? TOC_QUALITY_STRONG : TOC_QUALITY_MIXED;
    book->SetTocConfidence(quality, mapped, 0, unresolved);
  } else if (!structured_used) {
    PruneMobiFrontMatterTocCluster(book, reporter);

    MobiChapterQualityStats q;
    if (IsMobiHeuristicChapterSetNoisy(book, &q)) {
      book->ClearChapters();
      book->ClearTocConfidence();
      if (reporter) {
        char msg[224];
        snprintf(
            msg, sizeof(msg),
            "MOBI: TOC heuristic rejected ch=%u uniq=%u early=%u/%u noisy=%u "
            "structured=%u win<=%u",
            (unsigned)q.chapters, (unsigned)q.unique_pages,
            (unsigned)q.early_hits, (unsigned)q.chapters,
            (unsigned)(q.tiny_titles + q.noisy_titles),
            (unsigned)q.structured_titles, (unsigned)q.early_window);
        DBG_LOG(reporter, msg);
      }
    }
  }

  if (out) {
    out->mapped_chapters = mapped_chapters;
    out->structured_entries = structured_toc.size();
    out->structured_direct = structured_direct;
    out->structured_from_filepos = structured_from_filepos;
  }
}

static bool MobiInlineLooksLikeStructuredTitle(const std::string &title) {
  return LooksLikeStructuredMobiChapterTitle(title);
}

static std::string MobiInlineFoldLatin(const std::string &text) {
  return FoldLatinForMatch(text);
}

static size_t MobiInlineCountWords(const std::string &text) {
  return (size_t)CountAsciiWords(text);
}

static bool MobiInlineMostlyDigitsOrPunct(const std::string &text) {
  return IsMostlyDigitsOrPunctuation(text);
}

static bool PrepareMobiStructuredToc(const std::string &raw,
                                     const std::vector<u32> &offsets,
                                     u32 ncx_index, u32 encoding,
                                     const std::string *markup_utf8,
                                     u32 text_len,
                                     std::vector<MobiStructuredTocEntry> *out,
                                     bool *structured_from_filepos,
                                     IStatusReporter *reporter) {
  mobi_toc_resolver::PrepareCallbacks callbacks;
  callbacks.structured.decode_bytes_to_utf8 = DecodeMobiStructuredTocBytes;
  callbacks.structured.normalize_title = NormalizeMobiStructuredTocTitle;
  callbacks.structured.reject_title = RejectMobiStructuredTocTitle;
  callbacks.inline_title.looks_like_structured_title =
      MobiInlineLooksLikeStructuredTitle;
  callbacks.inline_title.fold_latin_for_match = MobiInlineFoldLatin;
  callbacks.inline_title.count_ascii_words = MobiInlineCountWords;
  callbacks.inline_title.is_mostly_digits_or_punctuation =
      MobiInlineMostlyDigitsOrPunct;
  callbacks.decode_bytes_to_utf8 = DecodeMobiStructuredTocBytes;
  return mobi_toc_resolver::PrepareStructuredToc(
      raw, offsets, ncx_index, encoding, markup_utf8, text_len, callbacks, out,
      structured_from_filepos, reporter);
}

static bool LoadDeferredMobiStructuredToc(
    const MobiDeferredState &state, std::vector<MobiStructuredTocEntry> *out,
    bool *structured_from_filepos, IStatusReporter *reporter) {
  mobi_toc_resolver::PrepareCallbacks callbacks;
  callbacks.structured.decode_bytes_to_utf8 = DecodeMobiStructuredTocBytes;
  callbacks.structured.normalize_title = NormalizeMobiStructuredTocTitle;
  callbacks.structured.reject_title = RejectMobiStructuredTocTitle;
  callbacks.inline_title.looks_like_structured_title =
      MobiInlineLooksLikeStructuredTitle;
  callbacks.inline_title.fold_latin_for_match = MobiInlineFoldLatin;
  callbacks.inline_title.count_ascii_words = MobiInlineCountWords;
  callbacks.inline_title.is_mostly_digits_or_punctuation =
      MobiInlineMostlyDigitsOrPunct;
  callbacks.decode_bytes_to_utf8 = DecodeMobiStructuredTocBytes;
  return mobi_toc_resolver::LoadDeferredStructuredToc(
      &state.structured_toc, state.have_structured_toc, state.markup_utf8,
      state.text_len_for_pos, state.source_path, callbacks, out,
      structured_from_filepos, reporter);
}

static bool DecodeHtmlEntity(const std::string &entity, std::string *out) {
  if (!out)
    return false;
  if (entity.empty())
    return false;
  if (entity[0] == '#') {
    unsigned long parsed = 0;
    if (entity.size() >= 2 && (entity[1] == 'x' || entity[1] == 'X')) {
      if (sscanf(entity.c_str() + 2, "%lx", &parsed) != 1)
        return false;
    } else {
      if (sscanf(entity.c_str() + 1, "%lu", &parsed) != 1)
        return false;
    }
    u32 cp = (u32)parsed;
    if (cp == 0)
      return false;
    AppendUtf8Codepoint(out, cp);
    return true;
  }

  if (entity == "amp")
    out->push_back('&');
  else if (entity == "lt")
    out->push_back('<');
  else if (entity == "gt")
    out->push_back('>');
  else if (entity == "quot")
    out->push_back('"');
  else if (entity == "apos")
    out->push_back('\'');
  else if (entity == "nbsp")
    out->push_back(' ');
  else if (entity == "mdash")
    AppendUtf8Codepoint(out, 0x2014);
  else if (entity == "ndash")
    AppendUtf8Codepoint(out, 0x2013);
  else if (entity == "hellip")
    AppendUtf8Codepoint(out, 0x2026);
  else
    return false;
  return true;
}

// html_to_text_map: if non-null, receives (html_byte_offset, text_byte_offset)
// samples captured every 256-512 bytes of HTML input.  Because this function strips tags,
// scripts, styles, and entities at highly non-uniform rates, a global linear
// ratio (html_pos / html_len) is a poor proxy for the corresponding text
// position.  The sampling table lets MobiHtmlPosToPage() do piecewise-linear
// interpolation instead.
enum MobiMarkupBlockItemType {
  MOBI_MARKUP_BLOCK_TEXT = 0,
  MOBI_MARKUP_BLOCK_HARD_BREAK,
  MOBI_MARKUP_BLOCK_IMAGE
};

struct MobiMarkupBlockItem {
  MobiMarkupBlockItemType type;
  std::string text;
  u16 image_id;
  size_t marker_offset;
};

struct MobiMarkupBlock {
  std::vector<MobiMarkupBlockItem> items;
};

static void AppendTextToMobiMarkupBlock(MobiMarkupBlock *block,
                                        const std::string &text) {
  if (!block || text.empty())
    return;
  if (!block->items.empty() &&
      block->items.back().type == MOBI_MARKUP_BLOCK_TEXT) {
    block->items.back().text += text;
    return;
  }
  MobiMarkupBlockItem item;
  item.type = MOBI_MARKUP_BLOCK_TEXT;
  item.text = text;
  item.image_id = 0;
  item.marker_offset = SIZE_MAX;
  block->items.push_back(item);
}

static void AppendHardBreakToMobiMarkupBlock(MobiMarkupBlock *block) {
  if (!block)
    return;
  if (!block->items.empty() &&
      block->items.back().type == MOBI_MARKUP_BLOCK_HARD_BREAK)
    return;
  MobiMarkupBlockItem item;
  item.type = MOBI_MARKUP_BLOCK_HARD_BREAK;
  item.image_id = 0;
  item.marker_offset = SIZE_MAX;
  block->items.push_back(item);
}

static void AppendImageToMobiMarkupBlock(MobiMarkupBlock *block, u16 image_id,
                                         size_t marker_offset) {
  if (!block)
    return;
  MobiMarkupBlockItem item;
  item.type = MOBI_MARKUP_BLOCK_IMAGE;
  item.image_id = image_id;
  item.marker_offset = marker_offset;
  block->items.push_back(item);
}

static bool MobiMarkupBlockItemHasMeaningfulText(const MobiMarkupBlockItem &item) {
  return item.type == MOBI_MARKUP_BLOCK_TEXT &&
         !TrimAsciiWhitespace(item.text).empty();
}

static bool MobiMarkupBlockItemIsMeaningful(const MobiMarkupBlockItem &item) {
  return item.type == MOBI_MARKUP_BLOCK_IMAGE ||
         MobiMarkupBlockItemHasMeaningfulText(item);
}

static bool MobiMarkupBlockHasMeaningfulContent(const MobiMarkupBlock &block) {
  for (size_t i = 0; i < block.items.size(); i++) {
    if (MobiMarkupBlockItemIsMeaningful(block.items[i]))
      return true;
  }
  return false;
}

static void FinalizeMobiMarkupBlock(std::vector<MobiMarkupBlock> *blocks,
                                    MobiMarkupBlock *current) {
  if (!blocks || !current)
    return;
  if (!MobiMarkupBlockHasMeaningfulContent(*current)) {
    current->items.clear();
    return;
  }
  blocks->push_back(*current);
  current->items.clear();
}

static int FindFirstMeaningfulMobiMarkupBlockItem(const MobiMarkupBlock &block) {
  for (size_t i = 0; i < block.items.size(); i++) {
    if (MobiMarkupBlockItemIsMeaningful(block.items[i]))
      return (int)i;
  }
  return -1;
}

static bool MobiMarkupBlockHasMeaningfulTextAfter(
    const MobiMarkupBlock &block, size_t start_index) {
  if (start_index >= block.items.size())
    return false;
  for (size_t i = start_index + 1; i < block.items.size(); i++) {
    if (MobiMarkupBlockItemHasMeaningfulText(block.items[i]))
      return true;
  }
  return false;
}

static bool MobiMarkupBlockHasMeaningfulTextBefore(
    const MobiMarkupBlock &block, size_t end_index) {
  if (end_index >= block.items.size())
    return false;
  for (size_t i = 0; i < end_index; i++) {
    if (MobiMarkupBlockItemHasMeaningfulText(block.items[i]))
      return true;
  }
  return false;
}

static int FindLastMeaningfulMobiMarkupBlockItem(const MobiMarkupBlock &block) {
  for (int i = (int)block.items.size() - 1; i >= 0; i--) {
    if (MobiMarkupBlockItemIsMeaningful(block.items[(size_t)i]))
      return i;
  }
  return -1;
}

static size_t CountMobiMarkupBlockImages(const MobiMarkupBlock &block) {
  size_t count = 0;
  for (size_t i = 0; i < block.items.size(); i++) {
    if (block.items[i].type == MOBI_MARKUP_BLOCK_IMAGE)
      count++;
  }
  return count;
}

static bool MobiMarkupBlockHasOnlyImage(const MobiMarkupBlock &block,
                                        size_t image_index) {
  if (image_index >= block.items.size())
    return false;
  int first = FindFirstMeaningfulMobiMarkupBlockItem(block);
  int last = FindLastMeaningfulMobiMarkupBlockItem(block);
  if (first < 0 || last < 0 || (size_t)first != image_index ||
      (size_t)last != image_index)
    return false;
  if (block.items[image_index].type != MOBI_MARKUP_BLOCK_IMAGE)
    return false;
  return true;
}

static std::string CollectMobiMarkupBlockText(const MobiMarkupBlock &block) {
  std::string text;
  for (size_t i = 0; i < block.items.size(); i++) {
    const MobiMarkupBlockItem &item = block.items[i];
    if (item.type == MOBI_MARKUP_BLOCK_TEXT) {
      text += item.text;
    } else if (item.type == MOBI_MARKUP_BLOCK_HARD_BREAK) {
      if (!text.empty() && text.back() != '\n')
        text.push_back('\n');
    }
  }
  return text;
}

static int EstimateWrappedLineCount(Text *ts, const std::string &text,
                                    int width) {
  if (text.empty())
    return 0;
  if (!ts || width <= 0)
    return 1;

  const int space_width = std::max(1, (int)ts->GetAdvance(' '));
  int lines = 1;
  int line_width = 0;
  int word_width = 0;
  bool have_word = false;
  bool pending_space = false;
  size_t i = 0;
  while (i < text.size()) {
    unsigned char c = (unsigned char)text[i];
    if (c == '\r') {
      i++;
      continue;
    }
    if (c == '\n') {
      if (have_word) {
        if (line_width == 0 || !pending_space) {
          line_width = (line_width == 0) ? word_width : (line_width + word_width);
        } else if (line_width + space_width + word_width <= width) {
          line_width += space_width + word_width;
        } else {
          lines++;
          line_width = word_width;
        }
        word_width = 0;
        have_word = false;
      }
      lines++;
      line_width = 0;
      pending_space = false;
      i++;
      continue;
    }
    if (isspace(c)) {
      if (have_word) {
        if (line_width == 0) {
          line_width = word_width;
        } else if (line_width + space_width + word_width <= width) {
          line_width += space_width + word_width;
        } else {
          lines++;
          line_width = word_width;
        }
        word_width = 0;
        have_word = false;
      }
      pending_space = true;
      i++;
      continue;
    }

    u32 cp = 0;
    size_t step = 1;
    if (c >= 0x80)
      step = (size_t)ts->GetCharCode(text.c_str() + i, text.size() - i, &cp);
    else
      cp = c;
    if (step == 0 || i + step > text.size()) {
      step = 1;
      cp = c;
    }
    word_width += std::max(1, (int)ts->GetAdvance(cp));
    have_word = true;
    i += step;
  }

  if (have_word) {
    if (line_width == 0) {
      line_width = word_width;
    } else if (pending_space && line_width + space_width + word_width <= width) {
      line_width += space_width + word_width;
    } else if (!pending_space && line_width + word_width <= width) {
      line_width += word_width;
    } else {
      lines++;
    }
  }

  return std::max(1, lines);
}

static bool StartsLikeListOrTitle(const std::string &text) {
  std::string compact = CollapseAsciiWhitespace(TrimAsciiWhitespace(text));
  if (compact.empty())
    return true;
  unsigned char first = (unsigned char)compact[0];
  if (first == '-' || first == '*' || first == 0xE2)
    return true;
  bool strong_signal = false;
  return LooksLikePlainChapterHeading(compact, &strong_signal);
}

static bool IsMobiCaptionLikeBlock(const MobiMarkupBlock &block, Text *ts,
                                   int text_width, int max_bytes,
                                   int max_lines) {
  if (!MobiMarkupBlockHasMeaningfulContent(block))
    return false;
  for (size_t i = 0; i < block.items.size(); i++) {
    if (block.items[i].type == MOBI_MARKUP_BLOCK_IMAGE)
      return false;
  }
  std::string raw = CollectMobiMarkupBlockText(block);
  std::string collapsed = CollapseAsciiWhitespace(TrimAsciiWhitespace(raw));
  if (collapsed.empty())
    return false;
  if ((int)collapsed.size() > max_bytes)
    return false;
  if (StartsLikeListOrTitle(collapsed))
    return false;
  return EstimateWrappedLineCount(ts, raw, text_width) <= max_lines;
}

static bool IsMobiFigureFollowerBlock(const MobiMarkupBlock &block) {
  if (!MobiMarkupBlockHasMeaningfulContent(block))
    return false;
  for (size_t i = 0; i < block.items.size(); i++) {
    if (block.items[i].type == MOBI_MARKUP_BLOCK_IMAGE)
      return false;
  }
  std::string raw = CollectMobiMarkupBlockText(block);
  std::string collapsed = CollapseAsciiWhitespace(TrimAsciiWhitespace(raw));
  if (collapsed.empty())
    return false;
  return !StartsLikeListOrTitle(collapsed);
}

static u8 EstimateMobiImageFollowTextLines(const MobiMarkupBlock &block, Text *ts,
                                           int text_width, size_t image_index) {
  std::string follow_text;
  for (size_t i = image_index + 1; i < block.items.size(); i++) {
    const MobiMarkupBlockItem &item = block.items[i];
    if (item.type == MOBI_MARKUP_BLOCK_TEXT) {
      follow_text += item.text;
    } else if (item.type == MOBI_MARKUP_BLOCK_HARD_BREAK) {
      if (!follow_text.empty() && follow_text.back() != '\n')
        follow_text.push_back('\n');
    }
  }
  std::string collapsed = CollapseAsciiWhitespace(TrimAsciiWhitespace(follow_text));
  if (collapsed.empty())
    return 0;
  int lines = EstimateWrappedLineCount(ts, follow_text, text_width);
  lines = std::max(1, std::min(lines, 8));
  return (u8)lines;
}

static void ApplyMobiImageContexts(Book *book,
                                   std::vector<MobiMarkupBlock> *blocks,
                                   std::string *out, Text *ts,
                                   int text_width) {
  if (!book || !blocks || !out)
    return;

  for (size_t bi = 0; bi < blocks->size(); bi++) {
    MobiMarkupBlock &block = (*blocks)[bi];
    const int first = FindFirstMeaningfulMobiMarkupBlockItem(block);
    const int last = FindLastMeaningfulMobiMarkupBlockItem(block);
    const size_t image_count = CountMobiMarkupBlockImages(block);
    if (first < 0 || last < 0 || image_count == 0)
      continue;

    for (size_t ii = 0; ii < block.items.size(); ii++) {
      MobiMarkupBlockItem &image = block.items[ii];
      if (image.type != MOBI_MARKUP_BLOCK_IMAGE)
        continue;

      const bool has_text_before =
          MobiMarkupBlockHasMeaningfulTextBefore(block, ii);
      const bool has_text_after =
          MobiMarkupBlockHasMeaningfulTextAfter(block, ii);
      const bool boundary_image = ((int)ii == first || (int)ii == last);
      bool figure_with_caption =
          image_count == 1 && boundary_image &&
          (has_text_before || has_text_after);
      u8 follow_text_lines = 0;

      if (figure_with_caption)
        follow_text_lines =
            EstimateMobiImageFollowTextLines(block, ts, text_width, ii);

      if (!figure_with_caption && MobiMarkupBlockHasOnlyImage(block, ii)) {
        for (size_t next = bi + 1; next < blocks->size(); next++) {
          if (!MobiMarkupBlockHasMeaningfulContent((*blocks)[next]))
            continue;
          const bool caption_like =
              IsMobiCaptionLikeBlock((*blocks)[next], ts, text_width, 220, 4);
          const bool figure_follower = IsMobiFigureFollowerBlock((*blocks)[next]);
          figure_with_caption = caption_like || figure_follower;
          if (figure_with_caption) {
            std::string raw = CollectMobiMarkupBlockText((*blocks)[next]);
            follow_text_lines =
                (u8)std::max(1, std::min(EstimateWrappedLineCount(ts, raw, text_width), 8));
          }
          break;
        }
      }

      if (!figure_with_caption)
        continue;
      // Rewrite the serialized marker in-place so deferred pagination and
      // rendering see the richer figure/caption context without changing the
      // html->text byte mapping used by TOC position interpolation.
      if (image.marker_offset != SIZE_MAX && image.marker_offset < out->size())
        (*out)[image.marker_offset] = (char)TEXT_IMAGE_FIGURE_WITH_CAPTION;
      if (follow_text_lines > 0)
        book->SetInlineImageFollowTextLines(image.image_id, follow_text_lines);
    }
  }
}

static std::string ExtractMobiMarkupToText(
    Book *book, const BookIoDeps &deps, const std::string &in,
    std::vector<MobiHeadingHint> *heading_hints,
    std::vector<std::pair<u32, u32>> *html_to_text_map) {
  std::string out;
  out.reserve(in.size());
  std::vector<MobiMarkupBlock> blocks;
  MobiMarkupBlock current_block;
  std::string block_pending_text;
  bool in_script = false;
  bool in_style = false;
  bool pending_space = false;
  bool at_paragraph_start = true;
  int heading_level = -1;
  std::string heading_text;

  // Build a sampling table mapping HTML byte offsets to output text offsets.
  const size_t kSampleInterval =
      mobi_position_map::HtmlSampleIntervalForTextBytes(in.size());
  size_t next_sample = 0;
  if (html_to_text_map) {
    html_to_text_map->clear();
    html_to_text_map->reserve(in.size() / kSampleInterval + 2);
    html_to_text_map->push_back({0, 0});
  }

  auto flush_block_text = [&]() {
    if (block_pending_text.empty())
      return;
    AppendTextToMobiMarkupBlock(&current_block, block_pending_text);
    block_pending_text.clear();
  };

  auto finalize_block = [&]() {
    flush_block_text();
    FinalizeMobiMarkupBlock(&blocks, &current_block);
  };

  auto append_pending_space = [&]() {
    if (!pending_space)
      return;
    AppendSingleSpace(&out);
    if (!out.empty() && out.back() == ' ')
      block_pending_text.push_back(' ');
    if (heading_level >= 0 && !heading_text.empty() &&
        heading_text.back() != ' ')
      heading_text.push_back(' ');
    pending_space = false;
    at_paragraph_start = false;
  };

  for (size_t i = 0; i < in.size();) {
    if (html_to_text_map && i >= next_sample) {
      html_to_text_map->push_back({(u32)i, (u32)out.size()});
      next_sample = i + kSampleInterval;
    }
    unsigned char c = (unsigned char)in[i];
    if (c == '<') {
      size_t close = in.find('>', i + 1);
      if (close == std::string::npos) {
        i++;
        continue;
      }

      const size_t tag_offset = i + 1;
      const size_t tag_length = close - tag_offset;
      i = close + 1;
      if (tag_length >= 3 && in[tag_offset] == '!' && in[tag_offset + 1] == '-' &&
          in[tag_offset + 2] == '-')
        continue;

      MobiMarkupTagInfo tag_info;
      if (!mobi_parse_markup_tag(in.data() + tag_offset, tag_length, &tag_info) ||
          !tag_info.valid) {
        continue;
      }

      if (tag_info.kind == MOBI_MARKUP_TAG_SCRIPT) {
        in_script = !tag_info.closing;
        continue;
      }
      if (tag_info.kind == MOBI_MARKUP_TAG_STYLE) {
        in_style = !tag_info.closing;
        continue;
      }
      if (in_script || in_style)
        continue;

      int tag_heading_level = tag_info.heading_level;
      if (!tag_info.closing && tag_heading_level >= 0) {
        heading_level = tag_heading_level;
        heading_text.clear();
      } else if (tag_info.closing && tag_heading_level >= 0 &&
                 heading_level >= 0) {
        std::string normalized =
            CollapseAsciiWhitespace(TrimAsciiWhitespace(heading_text));
        if (heading_hints && normalized.size() >= 3 &&
            normalized.size() <= 180) {
          if (heading_hints->empty() ||
              heading_hints->back().title != normalized ||
              heading_hints->back().level != (u8)heading_level) {
            MobiHeadingHint hint;
            hint.title = normalized;
            hint.level = (u8)heading_level;
            heading_hints->push_back(hint);
          }
        }
        heading_level = -1;
        heading_text.clear();
      }

      if (tag_info.kind == MOBI_MARKUP_TAG_BR) {
        flush_block_text();
        AppendHardBreakToMobiMarkupBlock(&current_block);
        out.push_back('\n');
        if (heading_level >= 0 && !heading_text.empty() &&
            heading_text.back() != ' ')
          heading_text.push_back(' ');
        pending_space = false;
        continue;
      }
      if (tag_info.kind == MOBI_MARKUP_TAG_IMG && !tag_info.closing) {
        u16 recindex = 0;
        if (book) {
          const std::string tag(in, tag_offset, tag_length);
          if (!mobi_extract_image_recindex(tag, &recindex))
            continue;
          if (pending_space) {
            AppendSingleSpace(&out);
            if (!out.empty() && out.back() == ' ')
              block_pending_text.push_back(' ');
            pending_space = false;
          }
          flush_block_text();
          u16 image_id =
              book->RegisterInlineImage(mobi_inline_image_path(recindex));
          size_t marker_offset = out.size();
          out.push_back((char)(at_paragraph_start
                                   ? TEXT_IMAGE_LEADING_PARAGRAPH
                                   : TEXT_IMAGE_CONTEXT_DEFAULT));
          out.push_back((char)TEXT_IMAGE);
          out.push_back((char)((image_id >> 8) & 0xFF));
          out.push_back((char)(image_id & 0xFF));
          AppendImageToMobiMarkupBlock(&current_block, image_id, marker_offset);
          at_paragraph_start = false;
        }
        continue;
      }
      if (tag_info.kind == MOBI_MARKUP_TAG_LI && !tag_info.closing) {
        finalize_block();
        AppendParagraphBreak(&out);
        out.append("- ");
        block_pending_text += "- ";
        pending_space = false;
        at_paragraph_start = false;
        continue;
      }
      if (tag_info.kind == MOBI_MARKUP_TAG_BLOCK) {
        finalize_block();
        AppendParagraphBreak(&out);
        pending_space = false;
        at_paragraph_start = true;
        if (!tag_info.closing && tag_heading_level >= 0) {
          const unsigned char marker =
              mobi_heading_markers::MarkerForHeadingLevel(tag_heading_level);
          if (marker != 0)
            out.push_back((char)marker);
        }
      }
      continue;
    }

    if (in_script || in_style) {
      i++;
      continue;
    }

    if (c == '&') {
      size_t semi = in.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 12) {
        std::string entity = in.substr(i + 1, semi - i - 1);
        std::string decoded;
        if (DecodeHtmlEntity(entity, &decoded)) {
          out += decoded;
          block_pending_text += decoded;
          if (heading_level >= 0)
            heading_text += decoded;
          pending_space = false;
          if (!decoded.empty() &&
              decoded.find_first_not_of(" \t\r\n") != std::string::npos) {
            at_paragraph_start = false;
          }
          i = semi + 1;
          continue;
        }
      }
    }

    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
      pending_space = true;
      while (i < in.size()) {
        i++;
        if (i >= in.size())
          break;
        c = (unsigned char)in[i];
        if (c != '\r' && c != '\n' && c != '\t' && c != ' ')
          break;
      }
      continue;
    }
    if (c < 0x20) {
      do {
        i++;
      } while (i < in.size() && (unsigned char)in[i] < 0x20 &&
               in[i] != '\r' && in[i] != '\n' && in[i] != '\t');
      continue;
    }

    if (c < 0x80) {
      append_pending_space();
      const size_t run_start = i;
      while (i < in.size()) {
        i++;
        if (i >= in.size())
          break;
        c = (unsigned char)in[i];
        if (c >= 0x80 || c == '<' || c == '&' || c == '\r' || c == '\n' ||
            c == '\t' || c == ' ' || c < 0x20)
          break;
      }
      const size_t run_len = i - run_start;
      out.append(in, run_start, run_len);
      block_pending_text.append(in, run_start, run_len);
      if (heading_level >= 0)
        heading_text.append(in, run_start, run_len);
      at_paragraph_start = false;
      continue;
    }

    append_pending_space();
    int step = 1;
    if ((c & 0xE0) == 0xC0)
      step = 2;
    else if ((c & 0xF0) == 0xE0)
      step = 3;
    else if ((c & 0xF8) == 0xF0)
      step = 4;
    if (i + (size_t)step > in.size())
      step = 1;
    out.append(in, i, (size_t)step);
    block_pending_text.append(in, i, (size_t)step);
    if (heading_level >= 0)
      heading_text.append(in, i, (size_t)step);
    at_paragraph_start = false;
    i += (size_t)step;
  }

  finalize_block();

  if (book && deps.ts) {
    Text *ts = deps.ts;
    const int text_width = 240 - ts->margin.left - ts->margin.right;
    ApplyMobiImageContexts(book, &blocks, &out, ts, text_width);
  }

  if (html_to_text_map)
    html_to_text_map->push_back({(u32)in.size(), (u32)out.size()});

  return out;
}

struct MobiHeaderInfo {
  std::vector<u32> offsets;
  u16 compression;
  u32 text_len;
  u32 text_rec_count;
  u32 encoding;
  u32 first_non_book_index;
  u32 mobi_full_name_off;
  u32 mobi_full_name_len;
  u32 huffcdic_record_index;
  u32 num_huffcdic_records;
  u32 trailing_flags;
  u32 ncx_index;

  MobiHeaderInfo()
      : compression(0), text_len(0), text_rec_count(0), encoding(1252),
        first_non_book_index(0), mobi_full_name_off(0),
        mobi_full_name_len(0), huffcdic_record_index(0),
        num_huffcdic_records(0), trailing_flags(0),
        ncx_index(kMobiNullIndex) {}
};

struct MobiDecodedText {
  std::string utf8;
  std::string text;
  std::vector<MobiHeadingHint> heading_hints;
  std::vector<std::pair<u32, u32>> html_to_text_map;
  bool toc_metadata_ready;
  bool used_utf8_guess;
  bool used_legacy_guess;

  MobiDecodedText()
      : toc_metadata_ready(false), used_utf8_guess(false),
        used_legacy_guess(false) {}
};

static u8 LoadMobiSource(const char *path, std::string *raw,
                         u64 *t_after_read) {
  if (!path || !raw)
    return BOOK_ERR_CORRUPT;
  raw->clear();
  if (!file_read_utils::ReadPathToStringLimited(path, raw, kMobiMaxBytes))
    return 252;
  if (raw->empty())
    return BOOK_ERR_CORRUPT;
  if (t_after_read)
    *t_after_read = osGetTime();
  return 0;
}

static u8 ParseMobiHeader(const std::string &raw, MobiHeaderInfo *header) {
  if (!header)
    return 254;

  header->offsets.clear();
  if (!mobi_record_scan::ParseRecordOffsets(raw, &header->offsets) ||
      header->offsets.size() < 3)
    return BOOK_ERR_CORRUPT;

  const u8 *data = (const u8 *)raw.data();
  const u32 rec0_start = header->offsets[0];
  const u32 rec0_end = header->offsets[1];
  if (rec0_end <= rec0_start || rec0_end - rec0_start < 16)
    return 254;

  const u8 *rec0 = data + rec0_start;
  const size_t rec0_len = (size_t)(rec0_end - rec0_start);
  mobi_record_decode::MobiRecord0Header rec0_header;
  if (!mobi_record_decode::ParseRecord0Header(rec0, rec0_len, &rec0_header))
    return 254;
  header->compression = rec0_header.compression;
  header->text_len = rec0_header.text_len;
  header->text_rec_count = rec0_header.text_rec_count;
  header->encoding = rec0_header.encoding;
  header->first_non_book_index = rec0_header.resource_start;
  header->mobi_full_name_off = rec0_header.title_offset;
  header->mobi_full_name_len = rec0_header.title_length;
  header->huffcdic_record_index = rec0_header.huffcdic_record_index;
  header->num_huffcdic_records = rec0_header.num_huffcdic_records;
  header->trailing_flags = rec0_header.trailing_flags;
  header->ncx_index = rec0_header.indx_index;

  u32 max_text_records = (u32)header->offsets.size() - 2;
  if (header->text_rec_count == 0 || header->text_rec_count > max_text_records)
    header->text_rec_count = max_text_records;
  if (header->first_non_book_index > 1) {
    u32 boundary = header->first_non_book_index - 1;
    if (boundary > 0 && boundary < header->text_rec_count)
      header->text_rec_count = boundary;
  }
  if (header->text_rec_count == 0)
    return 255;

  return 0;
}

static void ApplyMobiEmbeddedTitle(Book *book, const std::string &raw,
                                   const MobiHeaderInfo &header) {
  if (!book || header.mobi_full_name_len == 0 || header.mobi_full_name_len >= 2048)
    return;

  const u8 *data = (const u8 *)raw.data();
  const u32 rec0_start = header.offsets.empty() ? 0 : header.offsets[0];
  const u32 rec0_end = (header.offsets.size() > 1) ? header.offsets[1] : 0;
  if (rec0_end <= rec0_start || rec0_end - rec0_start < 16)
    return;

  const u8 *rec0 = data + rec0_start;
  const size_t rec0_len = (size_t)(rec0_end - rec0_start);
  std::string title_raw;
  bool title_ok = false;
  if ((size_t)header.mobi_full_name_off + (size_t)header.mobi_full_name_len <=
      rec0_len) {
    title_raw.assign((const char *)rec0 + header.mobi_full_name_off,
                     (size_t)header.mobi_full_name_len);
    title_ok = true;
  } else if ((size_t)16 + (size_t)header.mobi_full_name_off +
                 (size_t)header.mobi_full_name_len <=
             rec0_len) {
    title_raw.assign((const char *)rec0 + 16 + (size_t)header.mobi_full_name_off,
                     (size_t)header.mobi_full_name_len);
    title_ok = true;
  }
  if (!title_ok)
    return;

  std::string title_utf8 =
      DecodeMobiBytesToUtf8(title_raw, header.encoding, NULL, NULL);
  title_utf8 = CollapseAsciiWhitespace(TrimAsciiWhitespace(title_utf8));
  if (!title_utf8.empty() && title_utf8.size() < 220)
    book->SetTitle(title_utf8.c_str());
}

static bool BuildMobiMergedText(const std::string &raw,
                                const MobiHeaderInfo &header,
                                std::string *merged) {
  if (!merged)
    return false;
  mobi_record_decode::MobiRecord0Header rec0;
  rec0.compression = header.compression;
  rec0.text_len = header.text_len;
  rec0.text_rec_count = header.text_rec_count;
  rec0.encoding = header.encoding;
  rec0.resource_start = header.first_non_book_index;
  rec0.title_offset = header.mobi_full_name_off;
  rec0.title_length = header.mobi_full_name_len;
  rec0.huffcdic_record_index = header.huffcdic_record_index;
  rec0.num_huffcdic_records = header.num_huffcdic_records;
  rec0.trailing_flags = header.trailing_flags;
  rec0.indx_index = header.ncx_index;
  return mobi_record_decode::BuildMergedText(raw, header.offsets, rec0, merged);
}

static void CleanupDecodedMobiText(IStatusReporter *reporter, std::string *text,
                                   std::vector<std::pair<u32, u32>> *html_map,
                                   bool line_wrap_fix_applied) {
  if (!text)
    return;

  const size_t text_pre_cleanup = text->size();
  const bool have_map = html_map && !html_map->empty();
  std::string text_before_cleanup;
  if (have_map)
    text_before_cleanup = *text;

  NormalizeNewlines(text);
  const bool track_image_tokens = HasMobiInlineImageTokens(*text);
  std::string text_before_mobi_cleanup;
  std::vector<u16> image_ids_before_cleanup;
  if (track_image_tokens) {
    text_before_mobi_cleanup = *text;
    CollectMobiInlineImageTokenIds(*text, &image_ids_before_cleanup);
  }
  *text = mobi_text_cleanup::RepairCommonMojibakePreservingMobiImageTokens(
      *text);
  if (line_wrap_fix_applied) {
    *text = mobi_text_cleanup::FixBrokenParagraphWrapsPreservingMobiImageTokens(
        *text);
  }
  size_t text_post_cleanup = text->size();
  if (track_image_tokens) {
    std::vector<u16> image_ids_after_cleanup;
    CollectMobiInlineImageTokenIds(*text, &image_ids_after_cleanup);
    if (image_ids_before_cleanup != image_ids_after_cleanup) {
      *text = text_before_mobi_cleanup;
      text_post_cleanup = text->size();
    }
  }

  if (have_map && text_post_cleanup != text_pre_cleanup) {
    mobi_position_map::RemapHtmlToTextAfterCleanup(text_before_cleanup, *text,
                                                   html_map);
  }
}

static void BuildMobiTocMetadataFromUtf8(
    Book *book, const BookIoDeps &deps, const std::string &utf8,
    bool line_wrap_fix_applied, std::vector<MobiHeadingHint> *heading_hints,
    std::vector<std::pair<u32, u32>> *html_to_text_map) {
  if (!book || !heading_hints || !html_to_text_map)
    return;

  heading_hints->clear();
  html_to_text_map->clear();

  std::string text = ExtractMobiMarkupToText(book, deps, utf8, heading_hints,
                                             html_to_text_map);
  CleanupDecodedMobiText(deps.reporter, &text, html_to_text_map,
                         line_wrap_fix_applied);
}

static void DecodeAndCleanupMobiText(Book *book, const BookIoDeps &deps,
                                     const MobiHeaderInfo &header,
                                     const std::string &merged,
                                     bool collect_toc_metadata,
                                     MobiDecodedText *decoded,
                                     u64 *t_after_decode,
                                     u64 *t_after_markup_scan,
                                     u64 *t_after_cleanup,
                                     u64 *t_after_markup) {
  if (!decoded)
    return;

  decoded->utf8 = DecodeMobiBytesToUtf8(merged, header.encoding,
                                        &decoded->used_utf8_guess,
                                        &decoded->used_legacy_guess);
  if (t_after_decode)
    *t_after_decode = osGetTime();

  book->ClearInlineImages();
  decoded->text = ExtractMobiMarkupToText(
      book, deps, decoded->utf8,
      collect_toc_metadata ? &decoded->heading_hints : NULL,
      collect_toc_metadata ? &decoded->html_to_text_map : NULL);
  if (t_after_markup_scan)
    *t_after_markup_scan = osGetTime();
  CleanupDecodedMobiText(deps.reporter, &decoded->text,
                         collect_toc_metadata ? &decoded->html_to_text_map
                                              : NULL,
                         book->GetMobiLineWrapFix());
  if (t_after_cleanup)
    *t_after_cleanup = osGetTime();
  decoded->toc_metadata_ready = collect_toc_metadata;

  if (t_after_markup)
    *t_after_markup = osGetTime();
}

static void PrepareMobiDeferredState(const char *path,
                                     const MobiHeaderInfo &header,
                                     size_t merged_size, u64 t_parse_begin,
                                     u64 t_after_read, u64 t_after_decompress,
                                     u64 t_after_decode,
                                     u64 t_after_markup_scan,
                                     u64 t_after_cleanup, u64 t_after_markup,
                                     bool line_wrap_fix_applied,
                                     bool retain_markup_utf8,
                                     MobiDecodedText *decoded,
                                     MobiDeferredState *deferred) {
  if (!decoded || !deferred)
    return;

  deferred->source_path = path ? path : "";
  if (retain_markup_utf8)
    deferred->markup_utf8.swap(decoded->utf8);
  else
    deferred->markup_utf8.clear();
  deferred->text_utf8.swap(decoded->text);
  deferred->heading_hints.swap(decoded->heading_hints);
  deferred->html_to_text_map.swap(decoded->html_to_text_map);
  deferred->toc_metadata_ready = decoded->toc_metadata_ready;
  deferred->structured_toc_loaded = false;
  deferred->toc_applied = false;
  deferred->cache_saved = false;
  deferred->used_utf8_guess = decoded->used_utf8_guess;
  deferred->used_legacy_guess = decoded->used_legacy_guess;
  deferred->line_wrap_fix_applied = line_wrap_fix_applied;
  deferred->finalized = false;
  deferred->text_len_for_pos =
      (header.text_len > 0) ? header.text_len : (u32)merged_size;
  deferred->t_parse_begin = t_parse_begin;
  deferred->t_after_read = t_after_read;
  deferred->t_after_decompress = t_after_decompress;
  deferred->t_after_decode = t_after_decode;
  deferred->t_after_markup_scan = t_after_markup_scan;
  deferred->t_after_cleanup = t_after_cleanup;
  deferred->t_after_initial_pages = 0;
  deferred->t_after_markup = t_after_markup;
  deferred->t_after_pages = 0;
  deferred->t_after_toc = 0;
}

static bool StartInitialMobiPagination(Book *book, const BookIoDeps &deps,
                                       MobiDeferredState *deferred,
                                       bool *pages_done_initial) {
  if (!deferred || !pages_done_initial)
    return false;
  if (!InitPlainTextStreamState(book, deferred->text_utf8, deps, false,
                                &deferred->stream)) {
    return false;
  }
  book->MarkMobiRenderSettingsApplied(deferred->line_wrap_fix_applied);
  PlainTextStreamPerf perf;
  *pages_done_initial = ContinuePlainTextStreamState(
      &deferred->stream, deferred->text_utf8, kMobiInitialOpenBudgetMs,
      kMobiInitialOpenPageBudget, 1,
      &deferred->text_cursor_per_page, &perf);
#ifdef DSLIBRIS_DEBUG
  LogPlainTextStreamPerf(deps.reporter, "PLAIN-MOBI initial", perf,
                         *pages_done_initial);
#endif
  deferred->t_after_initial_pages = osGetTime();
  deferred->t_after_pages = deferred->t_after_initial_pages;
  return true;
}

static void FinalizeImmediateMobiParse(Book *book, const char *path,
                                       const BookIoDeps &deps,
                                       const std::string &raw,
                                       const MobiHeaderInfo &header,
                                       const std::string &utf8,
                                       MobiDeferredState *deferred,
                                       MobiTocFinalizeResult *toc_result) {
  if (!book || !deferred)
    return;

  IStatusReporter *reporter = deps.reporter;
  MobiTocFinalizeResult local_result;
  if (!toc_result)
    toc_result = &local_result;

  if (!deferred->toc_metadata_ready) {
    const std::string &markup_source =
        !deferred->markup_utf8.empty() ? deferred->markup_utf8 : utf8;
    BuildMobiTocMetadataFromUtf8(book, deps, markup_source,
                                 deferred->line_wrap_fix_applied,
                                 &deferred->heading_hints,
                                 &deferred->html_to_text_map);
    deferred->toc_metadata_ready = true;
  }

  if (!deferred->have_structured_toc) {
    deferred->have_structured_toc = PrepareMobiStructuredToc(
        raw, header.offsets, header.ncx_index, header.encoding, &utf8,
        deferred->text_len_for_pos, &deferred->structured_toc,
        &deferred->structured_from_filepos, reporter);
  }
  FinalizeMobiPreparedToc(
      book, reporter, deferred->structured_toc, deferred->have_structured_toc,
      deferred->structured_from_filepos, deferred->heading_hints,
      deferred->text_len_for_pos, deferred->html_to_text_map,
      deferred->text_cursor_per_page, toc_result);
  deferred->t_after_toc = osGetTime();
  SaveMobiPageCache(book, path, deps, deferred->line_wrap_fix_applied);
  book->MarkMobiRenderSettingsApplied(deferred->line_wrap_fix_applied);
}

static u8 ParseMobiFile(Book *book, const char *path) {
  if (!book || !path)
    return 251;
  const BookIoDeps deps = BuildBookIoDeps(book);
  IStatusReporter *reporter = deps.reporter;
  const u64 t_parse_begin = osGetTime();
#ifdef DSLIBRIS_DEBUG
  buffered_status_log::BufferedStatusLog debug_log(768);
  struct DebugLogGuard {
    IStatusReporter *reporter;
    buffered_status_log::BufferedStatusLog *log;
    ~DebugLogGuard() { FlushBufferedStatusLog(reporter, log); }
  } debug_log_guard = {reporter, &debug_log};
  auto append_debug_log = [&](const std::string &line) {
    if (reporter)
      debug_log.Append(line);
  };
#else
  auto append_debug_log = [&](const std::string &) {};
#endif
  if (reporter)
    append_debug_log("MOBI: parse begin");

  g_mobi_deferred_states.erase(book);

  if (TryLoadMobiPageCache(book, path, deps)) {
    if (reporter) {
      char msg[224];
      snprintf(msg, sizeof(msg), "MOBI: page cache hit pages=%u chapters=%u",
               (unsigned)book->GetPageCount(),
               (unsigned)book->GetChapters().size());
      append_debug_log(msg);

      char tmsg[160];
      snprintf(tmsg, sizeof(tmsg), "MOBI: timing cache_total=%llums",
               (unsigned long long)(osGetTime() - t_parse_begin));
      append_debug_log(tmsg);
      append_debug_log("MOBI: parse end");
    }
    book->MarkMobiRenderSettingsApplied(book->GetMobiLineWrapFix());
    return 0;
  }

  std::string raw;
  u64 t_after_read = 0;
  u8 rc = LoadMobiSource(path, &raw, &t_after_read);
  if (rc != 0)
    return rc;

  MobiHeaderInfo header;
  rc = ParseMobiHeader(raw, &header);
  if (rc != 0) {
    if (rc == 255 && header.compression != 1 && header.compression != 2 &&
        header.compression != 17480 &&
        reporter) {
      if (header.compression == 17480)
        reporter->PrintStatus("MOBI: unsupported compression (HUFF/CDIC)");
      else
        reporter->PrintStatus("MOBI: unsupported compression");
    }
    return rc;
  }

  if (reporter) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "MOBI: header comp=%u enc=%u text_len=%u text_recs=%u "
             "first_non_book=%u ncx=%u",
             (unsigned)header.compression, (unsigned)header.encoding,
             (unsigned)header.text_len, (unsigned)header.text_rec_count,
             (unsigned)header.first_non_book_index,
             (unsigned)header.ncx_index);
    append_debug_log(msg);
  }

  if (header.compression != 1 && header.compression != 2 &&
      header.compression != 17480) {
    if (reporter) {
      if (header.compression == 17480)
        reporter->PrintStatus("MOBI: unsupported compression (HUFF/CDIC)");
      else
        reporter->PrintStatus("MOBI: unsupported compression");
    }
    return 255;
  }

  ApplyMobiEmbeddedTitle(book, raw, header);

  std::string merged;
  if (!BuildMobiMergedText(raw, header, &merged)) {
    if (reporter)
      reporter->PrintStatus("MOBI: failed to decode text records");
    return 255;
  }
  const u64 t_after_decompress = osGetTime();

  MobiDecodedText decoded;
  u64 t_after_decode = 0;
  u64 t_after_markup_scan = 0;
  u64 t_after_cleanup = 0;
  u64 t_after_markup = 0;
  const size_t text_bytes =
      (header.text_len > 0) ? (size_t)header.text_len : merged.size();
  const mobi_decode_plan::Plan decode_plan =
      mobi_decode_plan::Build(text_bytes);
  DecodeAndCleanupMobiText(
      book, deps, header, merged, decode_plan.capture_toc_metadata, &decoded,
      &t_after_decode, &t_after_markup_scan, &t_after_cleanup,
      &t_after_markup);

  MobiDeferredState deferred;
  PrepareMobiDeferredState(path, header, merged.size(), t_parse_begin,
                           t_after_read, t_after_decompress, t_after_decode,
                           t_after_markup_scan, t_after_cleanup,
                           t_after_markup, book->GetMobiLineWrapFix(),
                           decode_plan.retain_markup_utf8, &decoded,
                           &deferred);
  deferred.have_structured_toc = PrepareMobiStructuredToc(
      raw, header.offsets, header.ncx_index, header.encoding,
      &decoded.utf8, deferred.text_len_for_pos,
      &deferred.structured_toc, &deferred.structured_from_filepos, reporter);

  bool pages_done_initial = false;
  if (!StartInitialMobiPagination(book, deps, &deferred, &pages_done_initial))
    return 1;

  if (!pages_done_initial) {
    g_mobi_deferred_states[book] = std::move(deferred);
    if (reporter) {
      if (decode_plan.defer_toc_finalize) {
        DBG_LOGF(reporter,
                 "MOBI: deferred TOC finalize enabled text_bytes=%u threshold=%u",
                 (unsigned)text_bytes,
                 (unsigned)mobi_decode_plan::kDeferredTocFinalizeMinBytes);
      }
      DBG_LOGF(reporter,
               "MOBI: deferred open armed pages=%u initial_budget_ms=%u "
               "initial_page_budget=%u",
               (unsigned)book->GetPageCount(),
               (unsigned)kMobiInitialOpenBudgetMs,
               (unsigned)kMobiInitialOpenPageBudget);
      char tmsg[320];
      snprintf(tmsg, sizeof(tmsg),
               "MOBI: timing read=%llums decomp=%llums decode=%llums "
               "markup_scan=%llums cleanup=%llums initial_pages=%llums "
               "total_open=%llums",
               (unsigned long long)(t_after_read - t_parse_begin),
               (unsigned long long)(t_after_decompress - t_after_read),
               (unsigned long long)(t_after_decode - t_after_decompress),
               (unsigned long long)(t_after_markup_scan - t_after_decode),
               (unsigned long long)(t_after_cleanup - t_after_markup_scan),
               (unsigned long long)(deferred.t_after_initial_pages -
                                    t_after_cleanup),
               (unsigned long long)(deferred.t_after_initial_pages -
                                    t_parse_begin));
      append_debug_log(tmsg);
      append_debug_log("MOBI: parse end");
    }
    return 0;
  }

  MobiTocFinalizeResult toc_result;
  FinalizeImmediateMobiParse(book, path, deps, raw, header, decoded.utf8,
                             &deferred, &toc_result);

  if (reporter) {
    char msg[320];
    snprintf(
        msg, sizeof(msg),
        "MOBI: text bytes=%u headings=%u mapped=%u structured=%u direct=%u "
        "chapters=%u guess_utf8=%u guess_legacy=%u filepos_toc=%u",
        (unsigned)deferred.text_utf8.size(),
        (unsigned)deferred.heading_hints.size(),
        (unsigned)toc_result.mapped_chapters,
        (unsigned)toc_result.structured_entries,
        (unsigned)toc_result.structured_direct,
        (unsigned)book->GetChapters().size(),
        deferred.used_utf8_guess ? 1u : 0u,
        deferred.used_legacy_guess ? 1u : 0u,
        toc_result.structured_from_filepos ? 1u : 0u);
    append_debug_log(msg);

    char tmsg[320];
    snprintf(
        tmsg, sizeof(tmsg),
        "MOBI: timing read=%llums decomp=%llums decode=%llums "
        "markup_scan=%llums cleanup=%llums initial_pages=%llums "
        "deferred_pages=%llums deferred_toc=%llums total=%llums",
        (unsigned long long)(deferred.t_after_read - deferred.t_parse_begin),
        (unsigned long long)(deferred.t_after_decompress -
                             deferred.t_after_read),
        (unsigned long long)(deferred.t_after_decode -
                             deferred.t_after_decompress),
        (unsigned long long)(deferred.t_after_markup_scan -
                             deferred.t_after_decode),
        (unsigned long long)(deferred.t_after_cleanup -
                             deferred.t_after_markup_scan),
        (unsigned long long)(deferred.t_after_initial_pages -
                             deferred.t_after_cleanup),
        (unsigned long long)(deferred.t_after_pages -
                             deferred.t_after_initial_pages),
        (unsigned long long)(deferred.t_after_toc - deferred.t_after_pages),
        (unsigned long long)(deferred.t_after_toc - deferred.t_parse_begin));
    append_debug_log(tmsg);
  }

  if (reporter)
    append_debug_log("MOBI: parse end");
  return 0;
}

static bool FinalizeDeferredMobiState(Book *book, MobiDeferredState *state) {
  if (!book || !state)
    return true;
  if (state->finalized)
    return true;
  if (!state->stream.completed)
    return false;

  const BookIoDeps deps = BuildBookIoDeps(book);
  IStatusReporter *reporter = deps.reporter;
  switch (mobi_deferred_finalize_utils::NextFinalizeStage(
      state->stream.completed, state->toc_metadata_ready,
      state->structured_toc_loaded, state->toc_applied, state->cache_saved,
      state->finalized)) {
  case mobi_deferred_finalize_utils::FinalizeStage::BuildMetadata: {
    const u64 t_meta_begin = osGetTime();
    BuildMobiTocMetadataFromUtf8(book, deps, state->markup_utf8,
                                 state->line_wrap_fix_applied,
                                 &state->heading_hints,
                                 &state->html_to_text_map);
    state->toc_metadata_ready = true;
    if (reporter) {
      DBG_LOGF(reporter,
               "MOBI: deferred toc metadata ready ms=%llu headings=%u map=%u",
               (unsigned long long)(osGetTime() - t_meta_begin),
               (unsigned)state->heading_hints.size(),
               (unsigned)state->html_to_text_map.size());
    }
    return false;
  }
  case mobi_deferred_finalize_utils::FinalizeStage::LoadStructuredToc:
    // Deferred TOC resolution runs only once we already have the full page
    // map, which keeps initial open responsive even for large MOBIs.
    state->have_structured_toc = LoadDeferredMobiStructuredToc(
        *state, &state->structured_toc, &state->structured_from_filepos, reporter);
    state->structured_toc_loaded = true;
    return false;
  case mobi_deferred_finalize_utils::FinalizeStage::ApplyToc: {
    MobiTocFinalizeResult toc_result;
    FinalizeMobiPreparedToc(book, reporter, state->structured_toc,
                            state->have_structured_toc,
                            state->structured_from_filepos,
                            state->heading_hints, state->text_len_for_pos,
                            state->html_to_text_map,
                            state->text_cursor_per_page, &toc_result);
    state->t_after_toc = osGetTime();
    state->toc_applied = true;

    if (reporter) {
      char msg[320];
      snprintf(
          msg, sizeof(msg),
          "MOBI: text bytes=%u headings=%u mapped=%u structured=%u direct=%u "
          "chapters=%u guess_utf8=%u guess_legacy=%u filepos_toc=%u",
          (unsigned)state->text_utf8.size(),
          (unsigned)state->heading_hints.size(),
          (unsigned)toc_result.mapped_chapters,
          (unsigned)toc_result.structured_entries,
          (unsigned)toc_result.structured_direct,
          (unsigned)book->GetChapters().size(),
          state->used_utf8_guess ? 1u : 0u,
          state->used_legacy_guess ? 1u : 0u,
          toc_result.structured_from_filepos ? 1u : 0u);
      DBG_LOG(reporter, msg);

      char tmsg[320];
      snprintf(
          tmsg, sizeof(tmsg),
          "MOBI: timing read=%llums decomp=%llums decode=%llums "
          "markup_scan=%llums cleanup=%llums initial_pages=%llums "
          "deferred_pages=%llums deferred_toc=%llums total=%llums",
          (unsigned long long)(state->t_after_read - state->t_parse_begin),
          (unsigned long long)(state->t_after_decompress - state->t_after_read),
          (unsigned long long)(state->t_after_decode -
                               state->t_after_decompress),
          (unsigned long long)(state->t_after_markup_scan -
                               state->t_after_decode),
          (unsigned long long)(state->t_after_cleanup -
                               state->t_after_markup_scan),
          (unsigned long long)(state->t_after_initial_pages -
                               state->t_after_cleanup),
          (unsigned long long)(state->t_after_pages -
                               state->t_after_initial_pages),
          (unsigned long long)(state->t_after_toc - state->t_after_pages),
          (unsigned long long)(state->t_after_toc - state->t_parse_begin));
      DBG_LOG(reporter, tmsg);
    }
    return false;
  }
  case mobi_deferred_finalize_utils::FinalizeStage::SaveCache:
    SaveMobiPageCache(book, state->source_path.c_str(), deps,
                      state->line_wrap_fix_applied);
    book->MarkMobiRenderSettingsApplied(state->line_wrap_fix_applied);
    state->cache_saved = true;
    return false;
  case mobi_deferred_finalize_utils::FinalizeStage::Done:
    state->finalized = true;
    return true;
  case mobi_deferred_finalize_utils::FinalizeStage::ContinuePaging:
  default:
    return false;
  }
}

static bool ContinueDeferredMobiState(Book *book, MobiDeferredState *state,
                                      u32 budget_ms, u16 page_budget) {
  if (!book || !state)
    return true;

  const u16 pages_before = book->GetPageCount();
  PlainTextStreamPerf perf;
  const bool done = ContinuePlainTextStreamState(
      &state->stream, state->text_utf8, budget_ms, page_budget, 0,
      &state->text_cursor_per_page, &perf);
  if (book->GetPageCount() > pages_before)
    state->t_after_pages = osGetTime();

  if (!done)
    return false;
  return FinalizeDeferredMobiState(book, state);
}

} // namespace

u8 Book::Open() {
  PrepareForOpen();
  return OpenPrepared();
}

u8 Book::Index() {
  if (metadataIndexTried)
    return metadataIndexed ? 0 : 1;
  metadataIndexTried = true;

  int err = 1;
  if (format == FORMAT_EPUB) {
    std::string path;
    path.append(GetFolderName());
    path.append("/");
    path.append(GetFileName());
    err = epub(this, path, true);
  } else if (format == FORMAT_PDF) {
    std::string path;
    path.append(GetFolderName());
    path.append("/");
    path.append(GetFileName());
    err = IndexPdfMetadata(this, path.c_str());
  } else if (format == FORMAT_CBZ) {
    std::string path;
    path.append(GetFolderName());
    path.append("/");
    path.append(GetFileName());
    err = IndexCbzMetadata(this, path.c_str());
  } else {
    // Non-EPUB files currently use filename labels in browser; defer full parse
    // until open to keep startup responsive.
    err = 0;
  }
  if (!err) {
    metadataIndexed = true;
  }
  return err;
}

u8 Book::Parse(bool fulltext) {
  //! Parse full text (true) or titles only (false).
  //! Expat callback handlers do the heavy work.
  u8 rc = 0;

  char path[MAXPATHLEN];
  snprintf(path, sizeof(path), "%s/%s", GetFolderName(), GetFileName());

  // Lightweight non-XML formats.
  if (fulltext && HasExtCI(GetFileName(), ".txt"))
    return ParseTxtFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".rtf"))
    return ParseRtfFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".odt"))
    return odt_loader::ParseOdtFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".mobi"))
    return ParseMobiFile(this, path);
  if (fulltext && (HasExtCI(GetFileName(), ".pdf") ||
                   HasExtCI(GetFileName(), ".xps") ||
                   HasExtCI(GetFileName(), ".oxps")))
    return ParsePdfFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".cbz"))
    return ParseCbzFile(this, path);

  FILE *fp = fopen(path, "r");
  if (!fp) {
    rc = 255;
    return (rc);
  }

  const BookIoDeps deps = BuildBookIoDeps(this);
  parsedata_t parsedata;
  InitParsedataWithBookIoDeps(&parsedata, this, deps);
  parsedata.fb2_mode = fulltext && HasExtCI(GetFileName(), ".fb2");
  const u64 xml_parse_begin = osGetTime();
  const u16 xml_pages_before = GetPageCount();
  PlainTextPerfBaseline xml_perf_baseline;
  CapturePlainTextPerfBaseline(parsedata, &xml_perf_baseline);

  xml_parse_utils::XmlParserOptions options;
  options.user_data = &parsedata;
  options.default_handler = xml::book::fallback;
  options.processing_instruction = xml::book::instruction;
  options.start_element = xml::book::start;
  options.end_element = xml::book::end;
  options.character_data = xml::book::chardata;
  if (!fulltext) {
    options.start_element = xml::book::metadata::start;
    options.end_element = xml::book::metadata::end;
    options.character_data = xml::book::metadata::chardata;
  }
  xml_parse_utils::XmlParseResult parse_result =
      xml_parse_utils::ParseXmlFileStream(
          fp, options, parser_limits::kXmlStreamBufferSize,
          [](void *user_data) {
            parsedata_t *parsedata = static_cast<parsedata_t *>(user_data);
            return parsedata && parsedata->status;
          });
  fclose(fp);
  if (!parse_result.ok) {
    if (deps.reporter)
      deps.reporter->PrintStatus(
          xml_parse_utils::FormatXmlParseError(parse_result).c_str());
    rc = 254;
  }

  if (rc == 0 && fulltext && parsedata.fb2_mode) {
    bool has_structured_toc = !chapters.empty();
    if (!has_structured_toc)
      BuildFb2FallbackChapters(this);
    if (!chapters.empty())
      SetNonEpubTocConfidence(this, has_structured_toc);
    else
      ClearTocConfidence();
  }

#ifdef DSLIBRIS_DEBUG
  if (deps.reporter && fulltext) {
    PlainTextStreamPerf perf;
    FillPlainTextStreamPerf(parsedata, xml_perf_baseline,
                            osGetTime() - xml_parse_begin, 0,
                            GetPageCount() - xml_pages_before, &perf);
    LogPlainTextStreamPerf(
        deps.reporter, parsedata.fb2_mode ? "FB2 layout" : "XML layout", perf,
        rc == 0);
  }
#endif

  return (rc);
}

bool Book::HasDeferredMobiParse() const {
  return g_mobi_deferred_states.find(this) != g_mobi_deferred_states.end();
}

bool Book::ContinueDeferredMobiParse(u32 budget_ms, u16 page_budget) {
  auto it = g_mobi_deferred_states.find(this);
  if (it == g_mobi_deferred_states.end())
    return true;

  if (!ContinueDeferredMobiState(this, &it->second, budget_ms, page_budget))
    return false;

  g_mobi_deferred_states.erase(it);
  return true;
}

void Book::CancelDeferredMobiParse() { g_mobi_deferred_states.erase(this); }

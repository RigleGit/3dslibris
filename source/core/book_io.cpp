/*
    3dslibris - book_io.cpp
    New module for the 3DS port by Rigle.

    Summary:
    - Input/output and parser dispatch for non-EPUB formats.
    - UTF-8 normalization and encoding repair utilities.
    - TXT/RTF/ODT loading, extraction, and chapter/index helper generation.
*/

#include "book.h"

#include "epub.h"
#include "main.h"
#include "parse.h"
#include "unzip.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <vector>

namespace {

static const size_t kPlainTextMaxBytes = 12 * 1024 * 1024;
static const size_t kOdtContentMaxBytes = 24 * 1024 * 1024;
static const size_t kMobiMaxBytes = 64 * 1024 * 1024;
static const char *kMobiCacheBaseDir = "sdmc:/3ds/3dslibris/cache";
static const char *kMobiCacheDir = "sdmc:/3ds/3dslibris/cache/mobi";
static const u32 kMobiPageCacheMagic = 0x4D504347U; // "MPCG"
static const u16 kMobiPageCacheVersion = 1;

struct MobiPageCacheHeader {
  u32 magic;
  u16 version;
  u16 title_len;
  u32 page_count;
  u32 chapter_count;
  u8 toc_quality;
  u8 reserved0;
  u16 reserved1;
  u16 toc_direct;
  u16 toc_heuristic;
  u16 toc_unresolved;
};

static uint64_t Fnv1a64(const std::string &s) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < s.size(); i++) {
    hash ^= (uint8_t)s[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static void EnsureMobiCacheDirs() {
  static bool initialized = false;
  if (initialized)
    return;
  mkdir(kMobiCacheBaseDir, 0777);
  mkdir(kMobiCacheDir, 0777);
  initialized = true;
}

static std::string BuildMobiPageCachePath(const char *book_path, App *app) {
  if (!book_path || !app || !app->ts)
    return std::string();
  struct stat st;
  long long fsize = 0;
  long long fmtime = 0;
  if (stat(book_path, &st) == 0) {
    fsize = (long long)st.st_size;
    fmtime = (long long)st.st_mtime;
  }

  Text *ts = app->ts;
  std::string key(book_path);
  key.push_back('|');
  key += std::to_string(fsize);
  key.push_back('|');
  key += std::to_string(fmtime);
  key.push_back('|');
  key += std::to_string((int)ts->GetPixelSize());
  key.push_back('|');
  key += std::to_string((int)ts->linespacing);
  key.push_back('|');
  key += std::to_string((int)app->paraspacing);
  key.push_back('|');
  key += std::to_string((int)app->paraindent);
  key.push_back('|');
  key += std::to_string((int)app->orientation);
  key.push_back('|');
  key += std::to_string((int)ts->margin.left);
  key.push_back('|');
  key += std::to_string((int)ts->margin.right);
  key.push_back('|');
  key += std::to_string((int)ts->margin.top);
  key.push_back('|');
  key += std::to_string((int)ts->margin.bottom);
  key.push_back('|');
  key += ts->GetFontFile(TEXT_STYLE_REGULAR);

  uint64_t h = Fnv1a64(key);
  char out[192];
  snprintf(out, sizeof(out), "%s/%016llx.mpc",
           kMobiCacheDir, (unsigned long long)h);
  return std::string(out);
}

static bool TryLoadMobiPageCache(Book *book, const char *book_path, App *app) {
  if (!book || !book_path || !app)
    return false;
  EnsureMobiCacheDirs();
  std::string cache_path = BuildMobiPageCachePath(book_path, app);
  if (cache_path.empty())
    return false;

  FILE *fp = fopen(cache_path.c_str(), "rb");
  if (!fp)
    return false;

  MobiPageCacheHeader hdr;
  if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
    fclose(fp);
    remove(cache_path.c_str());
    return false;
  }
  if (hdr.magic != kMobiPageCacheMagic ||
      hdr.version != kMobiPageCacheVersion ||
      hdr.page_count == 0 || hdr.page_count > 10000 ||
      hdr.chapter_count > 4000 || hdr.title_len > 1000) {
    fclose(fp);
    remove(cache_path.c_str());
    return false;
  }

  std::string title;
  if (hdr.title_len > 0) {
    title.resize(hdr.title_len);
    if (fread(&title[0], 1, hdr.title_len, fp) != hdr.title_len) {
      fclose(fp);
      remove(cache_path.c_str());
      return false;
    }
  }

  bool ok = true;
  for (u32 i = 0; i < hdr.page_count; i++) {
    u16 len = 0;
    if (fread(&len, 1, sizeof(len), fp) != sizeof(len) || len > 4096) {
      ok = false;
      break;
    }
    std::vector<u8> buf((size_t)len);
    if (len > 0 && fread(buf.data(), 1, len, fp) != len) {
      ok = false;
      break;
    }
    Page *page = book->AppendPage();
    static u8 dummy = 0;
    page->SetBuffer(len ? buf.data() : &dummy, len);
  }

  if (ok) {
    for (u32 i = 0; i < hdr.chapter_count; i++) {
      u16 page = 0;
      u8 level = 0;
      u16 title_len = 0;
      if (fread(&page, 1, sizeof(page), fp) != sizeof(page) ||
          fread(&level, 1, sizeof(level), fp) != sizeof(level) ||
          fread(&title_len, 1, sizeof(title_len), fp) != sizeof(title_len) ||
          title_len > 2048) {
        ok = false;
        break;
      }
      std::string ctitle;
      ctitle.resize(title_len);
      if (title_len > 0 &&
          fread(&ctitle[0], 1, title_len, fp) != title_len) {
        ok = false;
        break;
      }
      if (page < book->GetPageCount())
        book->AddChapter(page, ctitle, level);
    }
  }

  fclose(fp);
  if (!ok) {
    book->Close();
    remove(cache_path.c_str());
    return false;
  }

  if (!title.empty())
    book->SetTitle(title.c_str());

  TocQuality q = TOC_QUALITY_UNKNOWN;
  if (hdr.toc_quality <= TOC_QUALITY_HEURISTIC)
    q = (TocQuality)hdr.toc_quality;
  book->SetTocConfidence(q, hdr.toc_direct, hdr.toc_heuristic,
                         hdr.toc_unresolved);
  return true;
}

static void SaveMobiPageCache(Book *book, const char *book_path, App *app) {
  if (!book || !book_path || !app || book->GetPageCount() == 0)
    return;
  EnsureMobiCacheDirs();
  std::string cache_path = BuildMobiPageCachePath(book_path, app);
  if (cache_path.empty())
    return;

  const char *title_c = book->GetTitle();
  std::string title = title_c ? title_c : "";
  if (title.size() > 1000)
    title.resize(1000);
  const std::vector<ChapterEntry> &chapters = book->GetChapters();

  MobiPageCacheHeader hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = kMobiPageCacheMagic;
  hdr.version = kMobiPageCacheVersion;
  hdr.title_len = (u16)title.size();
  hdr.page_count = book->GetPageCount();
  hdr.chapter_count = (u32)chapters.size();
  hdr.toc_quality = (u8)book->GetTocQuality();
  hdr.toc_direct = book->GetTocDirectCount();
  hdr.toc_heuristic = book->GetTocHeuristicCount();
  hdr.toc_unresolved = book->GetTocUnresolvedCount();

  FILE *fp = fopen(cache_path.c_str(), "wb");
  if (!fp)
    return;

  bool ok = fwrite(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr);
  if (ok && !title.empty())
    ok = fwrite(title.data(), 1, title.size(), fp) == title.size();

  if (ok) {
    for (u32 i = 0; i < hdr.page_count; i++) {
      Page *page = book->GetPage((int)i);
      u16 len = page ? (u16)page->GetLength() : 0;
      if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len)) {
        ok = false;
        break;
      }
      if (len > 0) {
        const u8 *buf = page->GetBuffer();
        if (!buf || fwrite(buf, 1, len, fp) != len) {
          ok = false;
          break;
        }
      }
    }
  }

  if (ok) {
    for (size_t i = 0; i < chapters.size(); i++) {
      const ChapterEntry &c = chapters[i];
      u16 page = c.page;
      u8 level = c.level;
      u16 title_len = (u16)std::min<size_t>(c.title.size(), 2048);
      if (fwrite(&page, 1, sizeof(page), fp) != sizeof(page) ||
          fwrite(&level, 1, sizeof(level), fp) != sizeof(level) ||
          fwrite(&title_len, 1, sizeof(title_len), fp) != sizeof(title_len)) {
        ok = false;
        break;
      }
      if (title_len > 0 &&
          fwrite(c.title.data(), 1, title_len, fp) != title_len) {
        ok = false;
        break;
      }
    }
  }

  fclose(fp);
  if (!ok)
    remove(cache_path.c_str());
}

static bool HasExtCI(const char *name, const char *ext) {
  if (!name || !ext)
    return false;
  size_t nlen = strlen(name);
  size_t elen = strlen(ext);
  if (elen == 0 || nlen < elen)
    return false;
  return strcasecmp(name + nlen - elen, ext) == 0;
}

static bool LooksLikeValidUtf8Bytes(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    if ((c & 0x80) == 0x00) {
      i++;
      continue;
    }
    size_t need = 0;
    if ((c & 0xE0) == 0xC0)
      need = 1;
    else if ((c & 0xF0) == 0xE0)
      need = 2;
    else if ((c & 0xF8) == 0xF0)
      need = 3;
    else
      return false;

    if (i + need >= s.size())
      return false;
    for (size_t j = 1; j <= need; j++) {
      unsigned char cc = (unsigned char)s[i + j];
      if ((cc & 0xC0) != 0x80)
        return false;
    }
    i += need + 1;
  }
  return true;
}

static void AppendUtf8Codepoint(std::string *out, u32 cp) {
  if (!out)
    return;
  if (cp <= 0x7F) {
    out->push_back((char)cp);
  } else if (cp <= 0x7FF) {
    out->push_back((char)(0xC0 | (cp >> 6)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out->push_back((char)(0xE0 | (cp >> 12)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    out->push_back((char)(0xF0 | (cp >> 18)));
    out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  }
}

static void AppendCp1252Byte(std::string *out, unsigned char b) {
  static const u16 cp1252_map[32] = {
      0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
      0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
  };

  if (!out)
    return;
  if (b < 0x80) {
    out->push_back((char)b);
    return;
  }
  if (b >= 0x80 && b <= 0x9F) {
    u16 mapped = cp1252_map[b - 0x80];
    if (mapped != 0x0000)
      AppendUtf8Codepoint(out, mapped);
    else
      out->push_back('?');
    return;
  }
  AppendUtf8Codepoint(out, b);
}

static std::string DecodeLegacySingleByteToUtf8(const std::string &in) {
  std::string out;
  out.reserve(in.size() * 2);
  for (size_t i = 0; i < in.size(); i++)
    AppendCp1252Byte(&out, (unsigned char)in[i]);
  return out;
}

static std::string NormalizeTextUtf8(const std::string &raw);
static std::string DecodeUtf16ToUtf8(const std::string &in);

static size_t CountUtf8InvalidLeadBytes(const std::string &bytes) {
  size_t invalid = 0;
  size_t i = 0;
  while (i < bytes.size()) {
    unsigned char c = (unsigned char)bytes[i];
    if ((c & 0x80) == 0x00) {
      i++;
      continue;
    }
    size_t need = 0;
    if ((c & 0xE0) == 0xC0)
      need = 1;
    else if ((c & 0xF0) == 0xE0)
      need = 2;
    else if ((c & 0xF8) == 0xF0)
      need = 3;
    else {
      invalid++;
      i++;
      continue;
    }
    if (i + need >= bytes.size()) {
      invalid++;
      i++;
      continue;
    }
    bool ok = true;
    for (size_t j = 1; j <= need; j++) {
      unsigned char cc = (unsigned char)bytes[i + j];
      if ((cc & 0xC0) != 0x80) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      invalid++;
      i++;
      continue;
    }
    i += need + 1;
  }
  return invalid;
}

static std::string DecodeMostlyUtf8WithCp1252Fallback(const std::string &in,
                                                      size_t *invalid_out) {
  if (invalid_out)
    *invalid_out = 0;
  std::string out;
  out.reserve(in.size() * 2);

  size_t i = 0;
  while (i < in.size()) {
    unsigned char c = (unsigned char)in[i];
    if ((c & 0x80) == 0x00) {
      out.push_back((char)c);
      i++;
      continue;
    }

    size_t need = 0;
    if ((c & 0xE0) == 0xC0)
      need = 1;
    else if ((c & 0xF0) == 0xE0)
      need = 2;
    else if ((c & 0xF8) == 0xF0)
      need = 3;

    bool valid = (need > 0 && i + need < in.size());
    if (valid) {
      for (size_t j = 1; j <= need; j++) {
        unsigned char cc = (unsigned char)in[i + j];
        if ((cc & 0xC0) != 0x80) {
          valid = false;
          break;
        }
      }
    }

    if (valid) {
      out.append(in, i, need + 1);
      i += need + 1;
      continue;
    }

    AppendCp1252Byte(&out, c);
    if (invalid_out)
      (*invalid_out)++;
    i++;
  }

  return out;
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
  score += CountNeedleOccurrences(utf8, "\xC3\x83");               // Ã
  score += CountNeedleOccurrences(utf8, "\xC3\x82");               // Â
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x82\xAC");   // â€
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x80\x99");   // â€™
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x80\x9C");   // â€œ
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x80\x9D");   // â€
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
  const size_t invalid_utf8_leads = raw_is_utf8 ? 0 : CountUtf8InvalidLeadBytes(in);
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

static std::string NormalizeTextUtf8(const std::string &raw) {
  std::string s = raw;
  if (s.size() >= 3 && (unsigned char)s[0] == 0xEF &&
      (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
    s.erase(0, 3);
  }
  if (LooksLikeValidUtf8Bytes(s))
    return s;
  return DecodeLegacySingleByteToUtf8(s);
}

static void NormalizeNewlines(std::string *s) {
  if (!s)
    return;
  std::string out;
  out.reserve(s->size());
  for (size_t i = 0; i < s->size(); i++) {
    char c = (*s)[i];
    if (c == '\r') {
      if (i + 1 < s->size() && (*s)[i + 1] == '\n')
        i++;
      out.push_back('\n');
    } else {
      out.push_back(c);
    }
  }
  s->swap(out);
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
      "chapter", "capitulo", "parte", "part", "seccion",
      "section", "book",     "libro",
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
    if (c != 'i' && c != 'v' && c != 'x' && c != 'l' && c != 'c' &&
        c != 'd' && c != 'm')
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
    if (c != 'I' && c != 'V' && c != 'X' && c != 'L' && c != 'C' &&
        c != 'D' && c != 'M')
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

static bool EndsWithStandaloneDigits(const std::string &folded, int *digits_out) {
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

static size_t PruneMobiFrontMatterTocCluster(Book *book, App *app) {
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

  if (app) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "MOBI: TOC front-matter pruned removed=%u remain=%u front_limit<=%u",
             (unsigned)prefix_count, (unsigned)kept.size(),
             (unsigned)front_page_limit);
    app->PrintStatus(msg);
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
  std::string first = (sp == std::string::npos) ? compact : compact.substr(0, sp);
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

static bool IsMobiHeuristicChapterSetNoisy(
    Book *book, MobiChapterQualityStats *stats_out) {
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

static bool ReadFileToStringLimited(const char *path, std::string *out,
                                    size_t max_bytes) {
  if (!path || !out)
    return false;
  out->clear();

  FILE *fp = fopen(path, "rb");
  if (!fp)
    return false;

  char buf[4096];
  while (true) {
    size_t n = fread(buf, 1, sizeof(buf), fp);
    if (n > 0) {
      if (out->size() + n > max_bytes) {
        fclose(fp);
        return false;
      }
      out->append(buf, n);
    }
    if (n < sizeof(buf)) {
      if (ferror(fp)) {
        fclose(fp);
        return false;
      }
      break;
    }
  }

  fclose(fp);
  return true;
}

static void FinalizePlainPage(parsedata_t *p) {
  if (!p || !p->book)
    return;
  if (p->buflen > 0 || p->book->GetPageCount() == 0) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
  }
}

struct PlainLineChunk {
  std::string text;
  bool has_newline;
  bool valid;
};

struct PlainTextStreamState {
  parsedata_t parsedata;
  size_t cursor;
  PlainLineChunk curr;
  PlainLineChunk next;
  bool prev_blank;
  bool prev_candidate;
  bool detect_heuristic_headings;
  bool initialized;
  bool completed;
};

static PlainLineChunk ReadNextLineChunk(const std::string &text, size_t *cursor) {
  PlainLineChunk out;
  out.text.clear();
  out.has_newline = false;
  out.valid = false;
  if (!cursor)
    return out;
  if (*cursor > text.size())
    return out;

  size_t start = *cursor;
  size_t end = text.find('\n', start);
  if (end == std::string::npos) {
    out.text = text.substr(start);
    out.has_newline = false;
    *cursor = text.size() + 1;
  } else {
    out.text = text.substr(start, end - start);
    out.has_newline = true;
    *cursor = end + 1;
  }
  out.valid = true;
  return out;
}

static bool InitPlainTextStreamState(Book *book, const std::string &text_utf8,
                                     bool detect_heuristic_headings,
                                     PlainTextStreamState *out) {
  if (!book || !book->GetApp() || !book->GetApp()->ts || !out)
    return false;

  out->cursor = 0;
  out->prev_blank = true;
  out->prev_candidate = false;
  out->detect_heuristic_headings = detect_heuristic_headings;
  out->initialized = false;
  out->completed = false;
  out->curr.text.clear();
  out->curr.has_newline = false;
  out->curr.valid = false;
  out->next.text.clear();
  out->next.has_newline = false;
  out->next.valid = false;
  parse_init(&out->parsedata);
  out->parsedata.app = book->GetApp();
  out->parsedata.ts = book->GetApp()->ts;
  out->parsedata.prefs = book->GetApp()->prefs;
  out->parsedata.book = book;
  parse_push(&out->parsedata, TAG_PRE);

  out->curr = ReadNextLineChunk(text_utf8, &out->cursor);
  out->next = ReadNextLineChunk(text_utf8, &out->cursor);
  out->initialized = true;
  return true;
}

static bool ContinuePlainTextStreamState(PlainTextStreamState *state,
                                         const std::string &text_utf8,
                                         u32 budget_ms, u16 page_budget,
                                         u16 min_pages_before_stop) {
  if (!state || !state->initialized || state->completed)
    return true;

  const u64 t_begin = osGetTime();
  const u16 page_start = state->parsedata.book->GetPageCount();

  while (state->curr.valid) {
    bool curr_blank = IsBlankLine(state->curr.text);
    bool next_blank = !state->next.valid || IsBlankLine(state->next.text);

    bool curr_strong = false;
    bool curr_candidate = false;
    bool next_strong = false;
    bool next_candidate = false;
    if (state->detect_heuristic_headings) {
      curr_candidate = LooksLikePlainChapterHeading(state->curr.text, &curr_strong);
      next_candidate = state->next.valid &&
                       LooksLikePlainChapterHeading(state->next.text, &next_strong);
    }

    if (state->detect_heuristic_headings && curr_candidate &&
        ShouldAcceptHeuristicHeading(state->curr.text, state->prev_blank,
                                     next_blank, state->prev_candidate,
                                     next_candidate, curr_strong)) {
      AddChapterIfUnique(state->parsedata.book, state->curr.text, 0);
    }

    if (!state->curr.text.empty()) {
      xml::book::chardata(&state->parsedata, state->curr.text.c_str(),
                          (int)state->curr.text.size());
    }
    if (state->curr.has_newline) {
      xml::book::chardata(&state->parsedata, "\n", 1);
    }

    state->prev_blank = curr_blank;
    state->prev_candidate = curr_candidate;
    state->curr = state->next;
    state->next = ReadNextLineChunk(text_utf8, &state->cursor);

    const u16 pages_done = state->parsedata.book->GetPageCount() - page_start;
    const bool have_min_pages =
        state->parsedata.book->GetPageCount() >= min_pages_before_stop;
    if (page_budget > 0 && pages_done >= page_budget && have_min_pages)
      break;
    if (budget_ms > 0 && (osGetTime() - t_begin) >= budget_ms && have_min_pages)
      break;
  }

  if (!state->curr.valid) {
    parse_pop(&state->parsedata);
    FinalizePlainPage(&state->parsedata);
    if (state->detect_heuristic_headings)
      SetNonEpubTocConfidence(state->parsedata.book, false);
    else
      state->parsedata.book->ClearTocConfidence();
    state->completed = true;
  }

  return state->completed;
}

static u8 ParsePlainTextBuffer(Book *book, const std::string &text_utf8,
                               bool detect_heuristic_headings = true) {
  if (!book)
    return 1;
  book->ClearChapters();
  book->ClearTocConfidence();

  PlainTextStreamState state;
  if (!InitPlainTextStreamState(book, text_utf8, detect_heuristic_headings,
                                &state))
    return 1;
  ContinuePlainTextStreamState(&state, text_utf8, 0, 0, 0);
  return 0;
}

static std::vector<std::string> ExtractTextLinesFromPage(Page *page) {
  std::vector<std::string> lines;
  if (!page)
    return lines;
  const u8 *buf = page->GetBuffer();
  const int len = page->GetLength();
  if (!buf || len <= 0)
    return lines;

  std::string line;
  line.reserve((size_t)len);
  int i = 0;
  while (i < len) {
    u8 c = buf[i];
    if (c == '\r' || c == '\n') {
      lines.push_back(line);
      line.clear();
      i++;
      continue;
    }
    if (c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF || c == TEXT_ITALIC_ON ||
        c == TEXT_ITALIC_OFF) {
      i++;
      continue;
    }
    if (c == TEXT_IMAGE) {
      if (!line.empty()) {
        lines.push_back(line);
        line.clear();
      }
      lines.push_back(std::string());
      if (i + 2 < len)
        i += 3;
      else
        i++;
      continue;
    }
    line.push_back((char)c);
    i++;
  }

  if (!line.empty() || lines.empty())
    lines.push_back(line);
  return lines;
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
        ExtractTextLinesFromPage(book->GetPage(page));
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

    if (curr_candidate &&
        ShouldAcceptHeuristicHeading(lines[i], prev_blank, next_blank,
                                     prev_candidate, next_candidate,
                                     curr_strong)) {
      AddChapterAtPageIfUnique(book, line_pages[i], lines[i], 0);
    }

    prev_blank = curr_blank;
    prev_candidate = curr_candidate;
  }
}

static int HexDigit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static std::string DecodeRtfToUtf8(const std::string &rtf) {
  std::string out;
  out.reserve(rtf.size());

  std::vector<bool> skip_stack;
  skip_stack.push_back(false);

  for (size_t i = 0; i < rtf.size();) {
    bool skip = skip_stack.back();
    char c = rtf[i];

    if (c == '{') {
      skip_stack.push_back(skip);
      i++;
      continue;
    }
    if (c == '}') {
      if (skip_stack.size() > 1)
        skip_stack.pop_back();
      i++;
      continue;
    }
    if (c != '\\') {
      if (!skip)
        out.push_back(c);
      i++;
      continue;
    }

    if (i + 1 >= rtf.size()) {
      i++;
      continue;
    }

    char n = rtf[i + 1];
    if (n == '\\' || n == '{' || n == '}') {
      if (!skip)
        out.push_back(n);
      i += 2;
      continue;
    }
    if (n == '*') {
      skip_stack.back() = true;
      i += 2;
      continue;
    }
    if (n == '\'') {
      if (i + 3 < rtf.size()) {
        int h1 = HexDigit(rtf[i + 2]);
        int h2 = HexDigit(rtf[i + 3]);
        if (h1 >= 0 && h2 >= 0 && !skip) {
          unsigned char b = (unsigned char)((h1 << 4) | h2);
          AppendCp1252Byte(&out, b);
        }
        i += 4;
      } else {
        i += 2;
      }
      continue;
    }
    if (n == '~') {
      if (!skip)
        out.push_back(' ');
      i += 2;
      continue;
    }
    if (n == '_') {
      if (!skip)
        out.push_back('-');
      i += 2;
      continue;
    }
    if (n == '-') {
      i += 2;
      continue;
    }
    if (n == 'u') {
      size_t p = i + 2;
      int sign = 1;
      if (p < rtf.size() && (rtf[p] == '-' || rtf[p] == '+')) {
        if (rtf[p] == '-')
          sign = -1;
        p++;
      }
      int value = 0;
      bool any = false;
      while (p < rtf.size() && isdigit((unsigned char)rtf[p])) {
        value = value * 10 + (rtf[p] - '0');
        p++;
        any = true;
      }
      if (any && !skip) {
        int cp = sign * value;
        if (cp < 0)
          cp += 65536;
        if (cp >= 0)
          AppendUtf8Codepoint(&out, (u32)cp);
      }
      if (p < rtf.size() && rtf[p] == ' ')
        p++;
      if (p < rtf.size() && rtf[p] != '\\' && rtf[p] != '{' && rtf[p] != '}')
        p++;
      i = p;
      continue;
    }
    if (isalpha((unsigned char)n)) {
      size_t p = i + 1;
      while (p < rtf.size() && isalpha((unsigned char)rtf[p]))
        p++;
      std::string word = rtf.substr(i + 1, p - (i + 1));
      if (p < rtf.size() && (rtf[p] == '-' || rtf[p] == '+' ||
                             isdigit((unsigned char)rtf[p]))) {
        p++;
        while (p < rtf.size() && isdigit((unsigned char)rtf[p]))
          p++;
      }
      if (p < rtf.size() && rtf[p] == ' ')
        p++;

      if (!skip) {
        if (word == "par" || word == "line") {
          out.push_back('\n');
        } else if (word == "tab") {
          out.push_back('\t');
        } else if (word == "emdash") {
          AppendUtf8Codepoint(&out, 0x2014);
        } else if (word == "endash") {
          AppendUtf8Codepoint(&out, 0x2013);
        } else if (word == "bullet") {
          AppendUtf8Codepoint(&out, 0x2022);
        } else if (word == "lquote") {
          AppendUtf8Codepoint(&out, 0x2018);
        } else if (word == "rquote") {
          AppendUtf8Codepoint(&out, 0x2019);
        } else if (word == "ldblquote") {
          AppendUtf8Codepoint(&out, 0x201C);
        } else if (word == "rdblquote") {
          AppendUtf8Codepoint(&out, 0x201D);
        }
      }
      i = p;
      continue;
    }

    i += 2;
  }

  return out;
}

static u8 ParseTxtFile(Book *book, const char *path) {
  std::string raw;
  if (!ReadFileToStringLimited(path, &raw, kPlainTextMaxBytes))
    return 252;
  NormalizeNewlines(&raw);
  std::string text = NormalizeTextUtf8(raw);
  return ParsePlainTextBuffer(book, text);
}

static u8 ParseRtfFile(Book *book, const char *path) {
  std::string raw;
  if (!ReadFileToStringLimited(path, &raw, kPlainTextMaxBytes))
    return 252;
  std::string text = DecodeRtfToUtf8(raw);
  NormalizeNewlines(&text);
  if (!LooksLikeValidUtf8Bytes(text))
    text = NormalizeTextUtf8(text);
  return ParsePlainTextBuffer(book, text);
}

static u16 ReadBE16(const u8 *p) {
  return (u16)((u16)p[0] << 8 | (u16)p[1]);
}

static u32 ReadBE32(const u8 *p) {
  return (u32)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (u32)p[3]);
}

static bool ParseMobiOffsets(const std::string &raw, std::vector<u32> *offsets) {
  if (!offsets || raw.size() < 78)
    return false;

  const u8 *buf = (const u8 *)raw.data();
  const u16 rec_count = ReadBE16(buf + 76);
  if (rec_count == 0 || raw.size() < 78 + (size_t)rec_count * 8)
    return false;

  offsets->assign((size_t)rec_count + 1, 0);
  for (u16 i = 0; i < rec_count; i++) {
    u32 off = ReadBE32(buf + 78 + (size_t)i * 8);
    if (off >= raw.size())
      return false;
    if (i > 0 && off < (*offsets)[(size_t)i - 1])
      return false;
    (*offsets)[i] = off;
  }
  (*offsets)[(size_t)rec_count] = (u32)raw.size();
  return true;
}

static const u32 kMobiNullIndex = 0xFFFFFFFFu;

struct MobiStructuredTocEntry {
  std::string title;
  u32 pos; // Raw MOBI filepos offset in merged text stream.
  u8 level;
};

struct MobiIndxHeader {
  u32 start;
  u32 count;
  u32 ncncx;
  u32 tagx;
  size_t header_end;
};

struct MobiTagXRule {
  u8 tag;
  u8 num_values;
  u8 bitmask;
  u8 eof;
};

struct MobiPendingTag {
  u8 tag;
  u8 num_values;
  u8 value_count;
  bool has_value_bytes;
  u32 value_bytes;
};

typedef std::unordered_map<u8, std::vector<u32>> MobiTagMap;

static int CountSetBits8(u8 v) {
  int count = 0;
  while (v) {
    count += (v & 1) ? 1 : 0;
    v >>= 1;
  }
  return count;
}

static bool ReadMobiVwiForward(const u8 *data, size_t len, size_t *consumed,
                               u32 *value) {
  if (!data || !consumed || !value || len == 0)
    return false;
  *consumed = 0;
  *value = 0;
  for (size_t i = 0; i < len; i++) {
    u8 b = data[i];
    *value = (*value << 7) | (u32)(b & 0x7F);
    *consumed = i + 1;
    if (b & 0x80)
      return true;
  }
  return false;
}

static size_t FindMobiSignature(const u8 *data, size_t len, const char *sig,
                                size_t sig_len, size_t start) {
  if (!data || !sig || sig_len == 0 || start >= len)
    return SIZE_MAX;
  for (size_t i = start; i + sig_len <= len; i++) {
    if (memcmp(data + i, sig, sig_len) == 0)
      return i;
  }
  return SIZE_MAX;
}

static bool GetMobiRecordSlice(const std::string &raw,
                               const std::vector<u32> &offsets, u32 rec_idx,
                               const u8 **data_out, size_t *len_out) {
  if (!data_out || !len_out || rec_idx + 1 >= offsets.size())
    return false;
  const u32 start = offsets[(size_t)rec_idx];
  const u32 end = offsets[(size_t)rec_idx + 1];
  if (end <= start || end > raw.size())
    return false;
  *data_out = (const u8 *)raw.data() + start;
  *len_out = (size_t)(end - start);
  return true;
}

static bool ParseMobiIndxHeader(const u8 *data, size_t len,
                                MobiIndxHeader *out) {
  if (!data || !out || len < 184)
    return false;
  if (memcmp(data, "INDX", 4) != 0)
    return false;
  out->start = ReadBE32(data + 20);
  out->count = ReadBE32(data + 24);
  out->ncncx = ReadBE32(data + 52);
  out->tagx = ReadBE32(data + 180);
  out->header_end = 184;
  return true;
}

static bool ParseMobiTagxSection(const u8 *data, size_t len, size_t tagx_start,
                                 u32 *control_bytes,
                                 std::vector<MobiTagXRule> *rules) {
  if (!data || !control_bytes || !rules || tagx_start >= len)
    return false;
  rules->clear();
  *control_bytes = 0;

  if (tagx_start + 12 > len || memcmp(data + tagx_start, "TAGX", 4) != 0)
    return false;

  const u32 first_entry_off = ReadBE32(data + tagx_start + 4);
  *control_bytes = ReadBE32(data + tagx_start + 8);
  if (first_entry_off < 12 || tagx_start + first_entry_off > len)
    return false;

  for (size_t i = tagx_start + 12; i + 4 <= tagx_start + first_entry_off;
       i += 4) {
    MobiTagXRule r;
    r.tag = data[i + 0];
    r.num_values = data[i + 1];
    r.bitmask = data[i + 2];
    r.eof = data[i + 3];
    rules->push_back(r);
  }
  return !rules->empty();
}

static bool ParseMobiTagMap(const u8 *data, size_t len, u32 control_bytes,
                            const std::vector<MobiTagXRule> &rules,
                            MobiTagMap *out) {
  if (!data || !out)
    return false;
  out->clear();
  if (len == 0 || control_bytes > len)
    return false;

  const u8 *control = data;
  size_t cursor = (size_t)control_bytes;
  size_t control_index = 0;
  std::vector<MobiPendingTag> pending;
  pending.reserve(rules.size());

  for (size_t i = 0; i < rules.size(); i++) {
    const MobiTagXRule &rule = rules[i];
    if (rule.eof == 0x01) {
      control_index++;
      continue;
    }
    if (control_index >= control_bytes)
      break;

    u8 value = (u8)(control[control_index] & rule.bitmask);
    if (value == 0)
      continue;

    MobiPendingTag p;
    p.tag = rule.tag;
    p.num_values = (rule.num_values == 0) ? 1 : rule.num_values;
    p.value_count = 0;
    p.has_value_bytes = false;
    p.value_bytes = 0;

    if (value == rule.bitmask) {
      if (CountSetBits8(rule.bitmask) > 1) {
        size_t consumed = 0;
        u32 value_bytes = 0;
        if (!ReadMobiVwiForward(data + cursor, len - cursor, &consumed,
                                &value_bytes))
          return false;
        cursor += consumed;
        p.has_value_bytes = true;
        p.value_bytes = value_bytes;
      } else {
        p.value_count = 1;
      }
    } else {
      u8 mask = rule.bitmask;
      while ((mask & 0x01) == 0) {
        mask >>= 1;
        value >>= 1;
      }
      p.value_count = value;
    }
    pending.push_back(p);
  }

  for (size_t i = 0; i < pending.size(); i++) {
    const MobiPendingTag &p = pending[i];
    std::vector<u32> values;

    if (!p.has_value_bytes) {
      const size_t nvalues = (size_t)p.value_count * (size_t)p.num_values;
      for (size_t j = 0; j < nvalues; j++) {
        if (cursor >= len)
          return false;
        size_t consumed = 0;
        u32 v = 0;
        if (!ReadMobiVwiForward(data + cursor, len - cursor, &consumed, &v))
          return false;
        cursor += consumed;
        values.push_back(v);
      }
    } else {
      size_t consumed_total = 0;
      while (consumed_total < p.value_bytes) {
        if (cursor >= len)
          return false;
        size_t consumed = 0;
        u32 v = 0;
        if (!ReadMobiVwiForward(data + cursor, len - cursor, &consumed, &v))
          return false;
        cursor += consumed;
        consumed_total += consumed;
        values.push_back(v);
      }
      if (consumed_total != p.value_bytes)
        return false;
    }
    (*out)[p.tag] = values;
  }

  return true;
}

static void ParseMobiCncxRecords(
    const std::string &raw, const std::vector<u32> &offsets, u32 first_record,
    u32 record_count, u32 encoding,
    std::unordered_map<u32, std::string> *cncx_text) {
  if (!cncx_text)
    return;
  cncx_text->clear();
  for (u32 r = 0; r < record_count; r++) {
    const u32 rec_idx = first_record + r;
    const u8 *rec = NULL;
    size_t rec_len = 0;
    if (!GetMobiRecordSlice(raw, offsets, rec_idx, &rec, &rec_len))
      break;

    size_t pos = 0;
    while (pos < rec_len) {
      if (rec[pos] == 0)
        break;
      size_t consumed = 0;
      u32 slen = 0;
      if (!ReadMobiVwiForward(rec + pos, rec_len - pos, &consumed, &slen))
        break;

      const size_t text_start = pos + consumed;
      if (text_start + (size_t)slen > rec_len)
        break;

      std::string raw_txt((const char *)rec + text_start, (size_t)slen);
      std::string txt = DecodeMobiBytesToUtf8(raw_txt, encoding, NULL, NULL);
      txt = CollapseAsciiWhitespace(TrimAsciiWhitespace(txt));
      if (!txt.empty()) {
        const u32 key = (u32)pos + (r * 0x10000u);
        (*cncx_text)[key] = txt;
      }
      pos = text_start + (size_t)slen;
    }
  }
}

static std::string DecodeMobiIndexIdent(const u8 *entry, size_t len,
                                        u32 encoding, size_t *consumed) {
  if (consumed)
    *consumed = 0;
  if (!entry || len == 0)
    return std::string();
  const size_t ident_len = (size_t)entry[0];
  if (1 + ident_len > len)
    return std::string();
  if (consumed)
    *consumed = 1 + ident_len;
  std::string raw_ident((const char *)entry + 1, ident_len);
  std::string out = DecodeMobiBytesToUtf8(raw_ident, encoding, NULL, NULL);
  return CollapseAsciiWhitespace(TrimAsciiWhitespace(out));
}

static bool ParseMobiStructuredToc(const std::string &raw,
                                   const std::vector<u32> &offsets,
                                   u32 ncx_index, u32 encoding,
                                   std::vector<MobiStructuredTocEntry> *out,
                                   App *app) {
  if (!out)
    return false;
  out->clear();

  if (ncx_index == kMobiNullIndex || ncx_index + 1 >= offsets.size())
    return false;

  const u8 *main_rec = NULL;
  size_t main_len = 0;
  if (!GetMobiRecordSlice(raw, offsets, ncx_index, &main_rec, &main_len))
    return false;

  MobiIndxHeader main_hdr;
  if (!ParseMobiIndxHeader(main_rec, main_len, &main_hdr))
    return false;

  if (main_hdr.count == 0)
    return false;
  if ((size_t)ncx_index + (size_t)main_hdr.count + 1 >= offsets.size())
    return false;

  size_t tagx_start = (size_t)main_hdr.tagx;
  if (tagx_start + 4 > main_len ||
      memcmp(main_rec + tagx_start, "TAGX", 4) != 0) {
    size_t found =
        FindMobiSignature(main_rec, main_len, "TAGX", 4, main_hdr.header_end);
    if (found == SIZE_MAX)
      return false;
    tagx_start = found;
  }

  u32 control_bytes = 0;
  std::vector<MobiTagXRule> tag_rules;
  if (!ParseMobiTagxSection(main_rec, main_len, tagx_start, &control_bytes,
                            &tag_rules))
    return false;

  std::unordered_map<u32, std::string> cncx_text;
  if (main_hdr.ncncx > 0) {
    const u32 cncx_start = ncx_index + main_hdr.count + 1;
    ParseMobiCncxRecords(raw, offsets, cncx_start, main_hdr.ncncx, encoding,
                         &cncx_text);
  }

  size_t with_pos = 0;
  for (u32 rec_idx = ncx_index + 1; rec_idx <= ncx_index + main_hdr.count;
       rec_idx++) {
    const u8 *rec = NULL;
    size_t rec_len = 0;
    if (!GetMobiRecordSlice(raw, offsets, rec_idx, &rec, &rec_len))
      continue;

    MobiIndxHeader hdr;
    if (!ParseMobiIndxHeader(rec, rec_len, &hdr))
      continue;
    if (hdr.start + 4 > rec_len || memcmp(rec + hdr.start, "IDXT", 4) != 0)
      continue;
    if (hdr.count == 0)
      continue;

    const size_t idxt_entries_end = (size_t)hdr.start + 4 + (size_t)hdr.count * 2;
    if (idxt_entries_end > rec_len)
      continue;

    std::vector<size_t> idx_positions;
    idx_positions.reserve((size_t)hdr.count + 1);
    for (u32 i = 0; i < hdr.count; i++) {
      const size_t pos = (size_t)ReadBE16(rec + hdr.start + 4 + i * 2);
      if (pos >= rec_len || pos >= (size_t)hdr.start)
        continue;
      idx_positions.push_back(pos);
    }
    if (idx_positions.empty())
      continue;
    idx_positions.push_back((size_t)hdr.start);

    for (size_t i = 0; i + 1 < idx_positions.size(); i++) {
      const size_t start = idx_positions[i];
      const size_t end = idx_positions[i + 1];
      if (end <= start || end > rec_len)
        continue;

      size_t consumed = 0;
      std::string ident =
          DecodeMobiIndexIdent(rec + start, end - start, encoding, &consumed);
      if (consumed == 0 || start + consumed > end)
        continue;

      MobiTagMap tag_map;
      if (!ParseMobiTagMap(rec + start + consumed, end - (start + consumed),
                           control_bytes, tag_rules, &tag_map))
        continue;

      MobiStructuredTocEntry entry;
      entry.title = ident;
      entry.pos = kMobiNullIndex;
      entry.level = 0;

      std::unordered_map<u8, std::vector<u32>>::const_iterator it;
      it = tag_map.find(3); // Label offset in CNCX.
      if (it != tag_map.end() && !it->second.empty()) {
        std::unordered_map<u32, std::string>::const_iterator t =
            cncx_text.find(it->second[0]);
        if (t != cncx_text.end() && !t->second.empty())
          entry.title = t->second;
      }

      it = tag_map.find(1); // File position.
      if (it != tag_map.end() && !it->second.empty())
        entry.pos = it->second[0];
      if (entry.pos == kMobiNullIndex) {
        // Some generators store KF-style position tuples under tag 6.
        it = tag_map.find(6);
        if (it != tag_map.end() && it->second.size() >= 2)
          entry.pos = it->second[1];
      }

      it = tag_map.find(4); // Heading level.
      if (it != tag_map.end() && !it->second.empty()) {
        u32 lvl = it->second[0];
        entry.level = (lvl > 7) ? 7 : (u8)lvl;
      }

      entry.title = CollapseAsciiWhitespace(TrimAsciiWhitespace(entry.title));
      if (entry.title.empty() || IsMostlyDigitsOrPunctuation(entry.title))
        continue;

      if (!out->empty()) {
        const MobiStructuredTocEntry &last = out->back();
        if (last.pos == entry.pos && last.level == entry.level &&
            last.title == entry.title)
          continue;
      }

      if (entry.pos != kMobiNullIndex)
        with_pos++;
      out->push_back(entry);
    }
  }

  if (app && !out->empty()) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "MOBI: INDX structured entries=%u with_pos=%u cncx=%u",
             (unsigned)out->size(), (unsigned)with_pos,
             (unsigned)cncx_text.size());
    app->PrintStatus(msg);
  }

  return !out->empty();
}

static std::string DecodeUtf16ToUtf8(const std::string &in) {
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
    u16 w1 = little_endian ? ((u16)(unsigned char)in[i] |
                              ((u16)(unsigned char)in[i + 1] << 8))
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

static bool DecompressPalmDocRecord(const u8 *src, size_t src_len,
                                    std::string *out) {
  if (!src || !out)
    return false;
  out->clear();
  out->reserve(src_len * 2);

  size_t i = 0;
  while (i < src_len) {
    u8 c = src[i++];
    if (c == 0 || (c >= 0x09 && c <= 0x7F)) {
      out->push_back((char)c);
      continue;
    }
    if (c >= 0x01 && c <= 0x08) {
      size_t n = (size_t)c;
      if (i + n > src_len)
        n = src_len - i;
      out->append((const char *)(src + i), n);
      i += n;
      continue;
    }
    if (c >= 0xC0) {
      out->push_back(' ');
      out->push_back((char)(c ^ 0x80));
      continue;
    }
    if (i >= src_len)
      break;

    u8 c2 = src[i++];
    u16 pair = (u16)(((u16)c << 8) | (u16)c2);
    int distance = (pair >> 3) & 0x07FF;
    int length = (pair & 0x0007) + 3;
    if (distance <= 0 || (size_t)distance > out->size())
      continue;
    for (int j = 0; j < length; j++) {
      size_t idx = out->size() - (size_t)distance;
      out->push_back((*out)[idx]);
    }
  }
  return true;
}

static void AppendParagraphBreak(std::string *out) {
  if (!out)
    return;
  while (!out->empty() && (out->back() == ' ' || out->back() == '\t' ||
                           out->back() == '\r'))
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

static bool IsMobiBlockTag(const std::string &name) {
  static const char *kTags[] = {"p",     "div",      "section", "article",
                                "h1",    "h2",       "h3",      "h4",
                                "h5",    "h6",       "tr",      "table",
                                "li",    "ul",       "ol",      "blockquote",
                                "pre",   "header",   "footer",  "aside",
                                "title", "mbp:pagebreak"};
  for (size_t i = 0; i < sizeof(kTags) / sizeof(kTags[0]); i++) {
    if (name == kTags[i])
      return true;
  }
  return false;
}

struct MobiHeadingHint {
  std::string title;
  u8 level;
};

struct MobiDeferredState {
  PlainTextStreamState stream;
  std::string source_path;
  std::string text_utf8;
  std::vector<MobiHeadingHint> heading_hints;
  std::vector<MobiStructuredTocEntry> structured_toc;
  bool have_structured_toc;
  bool structured_from_filepos;
  bool used_utf8_guess;
  bool used_legacy_guess;
  bool finalized;
  u32 text_len_for_pos;
  u64 t_parse_begin;
  u64 t_after_read;
  u64 t_after_decompress;
  u64 t_after_decode;
  u64 t_after_markup;
  u64 t_after_pages;
  u64 t_after_toc;
};

static std::unordered_map<const Book *, MobiDeferredState> g_mobi_deferred_states;

static int MobiHeadingTagLevel(const std::string &name) {
  if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6')
    return (name[1] - '1');
  return -1;
}

static std::string NormalizeHeadingNeedle(const std::string &s) {
  return CollapseAsciiWhitespace(FoldLatinForMatch(TrimAsciiWhitespace(s)));
}

static bool PageHasHeadingNeedle(const std::vector<std::string> &lines,
                                 const std::string &needle) {
  if (needle.empty())
    return false;
  const size_t cap = std::min((size_t)18, lines.size());
  for (size_t i = 0; i < cap; i++) {
    std::string norm = NormalizeHeadingNeedle(lines[i]);
    if (norm.empty())
      continue;
    if (norm == needle)
      return true;
    if (norm.size() > needle.size() && norm.find(needle) != std::string::npos)
      return true;
  }
  return false;
}

static size_t BuildMobiChaptersFromHints(Book *book,
                                         const std::vector<MobiHeadingHint> &hints) {
  if (!book || hints.empty() || book->GetPageCount() == 0)
    return 0;

  std::vector<std::vector<std::string>> page_lines;
  page_lines.resize(book->GetPageCount());
  for (u16 p = 0; p < book->GetPageCount(); p++)
    page_lines[p] = ExtractTextLinesFromPage(book->GetPage(p));

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

static int FindMobiHeadingNearPage(
    const std::vector<std::vector<std::string>> &page_lines,
    const std::string &needle, u16 guess_page, u16 radius) {
  if (needle.size() < 3 || page_lines.empty())
    return -1;
  const u16 page_count = (u16)page_lines.size();
  if (guess_page >= page_count)
    guess_page = page_count - 1;

  for (u16 delta = 0; delta <= radius; delta++) {
    int lo = (int)guess_page - (int)delta;
    if (lo >= 0 && PageHasHeadingNeedle(page_lines[(size_t)lo], needle))
      return lo;
    if (delta == 0)
      continue;
    u16 hi = (u16)(guess_page + delta);
    if (hi < page_count && PageHasHeadingNeedle(page_lines[(size_t)hi], needle))
      return (int)hi;
  }
  return -1;
}

static size_t BuildMobiChaptersFromStructuredToc(
    Book *book, const std::vector<MobiStructuredTocEntry> &entries,
    u32 text_len, size_t *direct_out, bool refine_with_heading_search) {
  if (direct_out)
    *direct_out = 0;
  if (!book || entries.empty() || book->GetPageCount() == 0)
    return 0;

  const u16 page_count = book->GetPageCount();
  bool needs_heading_search = refine_with_heading_search;
  if (!needs_heading_search) {
    for (size_t i = 0; i < entries.size(); i++) {
      if (entries[i].pos == kMobiNullIndex) {
        needs_heading_search = true;
        break;
      }
    }
  }

  std::vector<std::vector<std::string>> page_lines;
  if (needs_heading_search) {
    page_lines.resize(page_count);
    for (u16 p = 0; p < page_count; p++)
      page_lines[p] = ExtractTextLinesFromPage(book->GetPage(p));
  }

  std::set<u16> used_pages;
  size_t mapped = 0;
  size_t direct_used = 0;
  u16 scan_start = 0;
  const u32 denom = (text_len > 0) ? text_len : 1;

  for (size_t i = 0; i < entries.size(); i++) {
    std::string clean = CollapseAsciiWhitespace(TrimAsciiWhitespace(entries[i].title));
    if (clean.empty())
      continue;

    const std::string needle = NormalizeHeadingNeedle(clean);
    bool has_pos = (entries[i].pos != kMobiNullIndex);
    int best_page = -1;

    if (has_pos) {
      double ratio = (double)entries[i].pos / (double)denom;
      if (ratio < 0.0)
        ratio = 0.0;
      if (ratio > 1.0)
        ratio = 1.0;
      const u16 guess = (u16)(ratio * (double)(page_count - 1));
      best_page = (int)guess;

      // Refine around the expected position when title appears in heading lines.
      if (needs_heading_search) {
        int refined = FindMobiHeadingNearPage(page_lines, needle, guess, 32);
        if (refined >= 0)
          best_page = refined;
      }
    } else {
      if (!needs_heading_search)
        continue;
      for (u16 p = scan_start; p < page_count; p++) {
        if (PageHasHeadingNeedle(page_lines[p], needle)) {
          best_page = (int)p;
          break;
        }
      }
      if (best_page < 0 && scan_start > 0) {
        for (u16 p = 0; p < scan_start; p++) {
          if (PageHasHeadingNeedle(page_lines[p], needle)) {
            best_page = (int)p;
            break;
          }
        }
      }
    }

    if (best_page < 0 || best_page >= page_count)
      continue;

    const u16 page = (u16)best_page;
    if (used_pages.find(page) != used_pages.end())
      continue;

    AddChapterAtPageIfUnique(book, page, clean, entries[i].level);
    used_pages.insert(page);
    mapped++;
    scan_start = page;
    if (has_pos)
      direct_used++;
  }

  if (direct_out)
    *direct_out = direct_used;
  return mapped;
}

struct MobiTocFinalizeResult {
  size_t mapped_chapters;
  size_t structured_entries;
  size_t structured_direct;
  bool structured_from_filepos;
};

static void FinalizeMobiPreparedToc(
    Book *book, App *app, const std::vector<MobiStructuredTocEntry> &structured_toc,
    bool have_structured_toc, bool structured_from_filepos,
    const std::vector<MobiHeadingHint> &heading_hints, u32 text_len_for_pos,
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
    mapped_structured = BuildMobiChaptersFromStructuredToc(
        book, structured_toc, text_len_for_pos, &structured_direct,
        !structured_from_filepos);
    if (mapped_structured >= 2) {
      structured_used = true;
      PruneMobiFrontMatterTocCluster(book, app);
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
      if (app && structured_from_filepos) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "MOBI: filepos TOC mapped=%u direct=%u unresolved=%u",
                 (unsigned)mapped_structured, (unsigned)direct,
                 (unsigned)unresolved);
        app->PrintStatus(msg);
      }
    } else {
      book->ClearChapters();
      for (size_t i = 0; i < fallback.size(); i++) {
        book->AddChapter(fallback[i].page, fallback[i].title, fallback[i].level);
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
        book->AddChapter(fallback[i].page, fallback[i].title, fallback[i].level);
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
    TocQuality quality = (unresolved == 0) ? TOC_QUALITY_STRONG : TOC_QUALITY_MIXED;
    book->SetTocConfidence(quality, mapped, 0, unresolved);
  } else if (!structured_used) {
    PruneMobiFrontMatterTocCluster(book, app);

    MobiChapterQualityStats q;
    if (IsMobiHeuristicChapterSetNoisy(book, &q)) {
      book->ClearChapters();
      book->ClearTocConfidence();
      if (app) {
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "MOBI: TOC heuristic rejected ch=%u uniq=%u early=%u/%u noisy=%u "
                 "structured=%u win<=%u",
                 (unsigned)q.chapters, (unsigned)q.unique_pages,
                 (unsigned)q.early_hits, (unsigned)q.chapters,
                 (unsigned)(q.tiny_titles + q.noisy_titles),
                 (unsigned)q.structured_titles, (unsigned)q.early_window);
        app->PrintStatus(msg);
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

static size_t FindAsciiNoCase(const std::string &haystack, const char *needle,
                              size_t start, size_t limit) {
  if (!needle || !*needle)
    return std::string::npos;
  if (start >= haystack.size())
    return std::string::npos;
  if (limit > haystack.size())
    limit = haystack.size();
  if (start >= limit)
    return std::string::npos;

  const size_t nlen = strlen(needle);
  if (nlen == 0 || start + nlen > limit)
    return std::string::npos;

  for (size_t i = start; i + nlen <= limit; i++) {
    size_t j = 0;
    for (; j < nlen; j++) {
      unsigned char a = (unsigned char)haystack[i + j];
      unsigned char b = (unsigned char)needle[j];
      if (tolower(a) != tolower(b))
        break;
    }
    if (j == nlen)
      return i;
  }
  return std::string::npos;
}

static bool ParseMobiAnchorFilepos(const std::string &tag, u32 *out_pos) {
  if (!out_pos)
    return false;
  *out_pos = kMobiNullIndex;
  if (tag.empty())
    return false;

  size_t p = FindAsciiNoCase(tag, "filepos", 0, tag.size());
  if (p == std::string::npos)
    return false;
  p += 7;
  while (p < tag.size() && isspace((unsigned char)tag[p]))
    p++;
  if (p >= tag.size() || tag[p] != '=')
    return false;
  p++;
  while (p < tag.size() && isspace((unsigned char)tag[p]))
    p++;
  if (p >= tag.size())
    return false;

  char quote = 0;
  if (tag[p] == '\'' || tag[p] == '"')
    quote = tag[p++];
  if (p >= tag.size())
    return false;

  size_t d0 = p;
  while (p < tag.size() && isdigit((unsigned char)tag[p]))
    p++;
  if (p <= d0)
    return false;
  if (quote != 0 && (p >= tag.size() || tag[p] != quote))
    return false;

  unsigned long long parsed = 0;
  if (sscanf(tag.substr(d0, p - d0).c_str(), "%llu", &parsed) != 1)
    return false;
  if (parsed == 0ull || parsed > 0xFFFFFFFFull)
    return false;
  *out_pos = (u32)parsed;
  return true;
}

static std::string DecodeMobiAnchorInnerText(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool in_tag = false;
  bool pending_space = false;

  for (size_t i = 0; i < in.size();) {
    unsigned char c = (unsigned char)in[i];
    if (c == '<') {
      in_tag = true;
      pending_space = true;
      i++;
      continue;
    }
    if (in_tag) {
      if (c == '>')
        in_tag = false;
      i++;
      continue;
    }

    if (c == '&') {
      size_t semi = in.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 12) {
        std::string entity = in.substr(i + 1, semi - i - 1);
        std::string decoded;
        if (DecodeHtmlEntity(entity, &decoded)) {
          if (pending_space && !out.empty() && out.back() != ' ')
            out.push_back(' ');
          out += decoded;
          pending_space = false;
          i = semi + 1;
          continue;
        }
      }
    }

    if (isspace(c) || c < 0x20) {
      pending_space = true;
      i++;
      continue;
    }

    if (pending_space && !out.empty() && out.back() != ' ')
      out.push_back(' ');
    pending_space = false;

    if (c < 0x80) {
      out.push_back((char)c);
      i++;
      continue;
    }

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
    i += (size_t)step;
  }

  return CollapseAsciiWhitespace(TrimAsciiWhitespace(out));
}

static bool IsMobiLikelyTocTitle(const std::string &title) {
  std::string clean = CollapseAsciiWhitespace(TrimAsciiWhitespace(title));
  if (clean.size() < 3 || clean.size() > 180)
    return false;
  if (IsMostlyDigitsOrPunctuation(clean))
    return false;

  std::string folded = FoldLatinForMatch(clean);
  auto contains_ci = [&](const char *needle) -> bool {
    return (needle && *needle && folded.find(needle) != std::string::npos);
  };
  if (folded == "contents" || folded == "table of contents" ||
      folded == "indice" || folded == "indice de contenido" ||
      folded == "contenido")
    return false;
  if (folded == "index" || folded == "this page" || folded == "cover" ||
      folded == "title page" || folded == "copyright")
    return false;
  if (contains_ci("this page") || contains_ci("page") || contains_ci("back") ||
      contains_ci("next") || contains_ci("previous") || contains_ci("menu") ||
      contains_ci("search") || contains_ci("table of contents") ||
      contains_ci("contents"))
    return false;
  if (contains_ci("ep_prh_") || contains_ci("kindle:") || contains_ci("navpoint"))
    return false;

  // Avoid obvious long prose lines.
  if (CountAsciiWords(clean) > 20)
    return false;

  return true;
}

static int ScoreMobiTocTitle(const std::string &title) {
  std::string clean = CollapseAsciiWhitespace(TrimAsciiWhitespace(title));
  if (clean.empty())
    return -1000;

  int score = 0;
  const int words = CountAsciiWords(clean);
  if (words >= 2 && words <= 12)
    score += 20;
  if (words >= 13 && words <= 18)
    score += 8;
  if (words <= 1 || words > 20)
    score -= 30;

  if (LooksLikeStructuredMobiChapterTitle(clean))
    score += 40;

  std::string folded = FoldLatinForMatch(clean);
  if (folded == "this page" || folded == "index" || folded == "contents" ||
      folded == "table of contents")
    score -= 80;
  if (folded.find("this page") != std::string::npos ||
      folded.find(" table of contents") != std::string::npos)
    score -= 60;

  if ((int)clean.size() >= 8 && (int)clean.size() <= 80)
    score += 10;
  else if ((int)clean.size() < 5)
    score -= 25;

  return score;
}

static bool ParseMobiInlineFileposToc(const std::string &markup_utf8, u32 text_len,
                                      std::vector<MobiStructuredTocEntry> *out,
                                      App *app) {
  if (!out)
    return false;
  out->clear();
  if (markup_utf8.empty())
    return false;

  size_t full_scan_limit = markup_utf8.size();
  if (text_len > 0) {
    size_t hint = (size_t)text_len / 3;
    if (hint < 256 * 1024)
      hint = 256 * 1024;
    if (hint > 1024 * 1024)
      hint = 1024 * 1024;
    if (hint < full_scan_limit)
      full_scan_limit = hint;
  }
  if (full_scan_limit > markup_utf8.size())
    full_scan_limit = markup_utf8.size();

  size_t first_pass_limit = full_scan_limit;
  if (first_pass_limit > 512 * 1024)
    first_pass_limit = 384 * 1024;

  std::vector<MobiStructuredTocEntry> raw_entries;
  raw_entries.reserve(256);

  auto scan_for_entries = [&](size_t scan_limit) {
    size_t pos = 0;
    while (pos < scan_limit) {
      size_t a0 = markup_utf8.find("<a", pos);
      if (a0 == std::string::npos || a0 >= scan_limit)
        a0 = FindAsciiNoCase(markup_utf8, "<a", pos, scan_limit);
      if (a0 == std::string::npos)
        break;

      size_t tag_end = markup_utf8.find('>', a0 + 2);
      if (tag_end == std::string::npos || tag_end >= scan_limit)
        break;
      if (tag_end <= a0 + 2 || (tag_end - (a0 + 2)) > 640) {
        pos = tag_end + 1;
        continue;
      }

      std::string tag = markup_utf8.substr(a0 + 2, tag_end - (a0 + 2));
      u32 filepos = kMobiNullIndex;
      if (!ParseMobiAnchorFilepos(tag, &filepos)) {
        pos = tag_end + 1;
        continue;
      }

      if (filepos == 0 || filepos == kMobiNullIndex) {
        pos = tag_end + 1;
        continue;
      }
      if (text_len > 0) {
        u32 allowed_max = text_len + (text_len / 8) + 1024;
        if (filepos > allowed_max) {
          pos = tag_end + 1;
          continue;
        }
      }

      const size_t close_probe_limit =
          std::min(scan_limit, tag_end + (size_t)2048);
      size_t close = markup_utf8.find("</a", tag_end + 1);
      if (close == std::string::npos || close >= close_probe_limit)
        close = FindAsciiNoCase(markup_utf8, "</a", tag_end + 1,
                                close_probe_limit);
      if (close == std::string::npos || close <= tag_end) {
        pos = tag_end + 1;
        continue;
      }

      size_t inner_len = close - (tag_end + 1);
      if (inner_len > 512)
        inner_len = 512;
      std::string inner = markup_utf8.substr(tag_end + 1, inner_len);
      std::string title = DecodeMobiAnchorInnerText(inner);
      if (!IsMobiLikelyTocTitle(title)) {
        pos = close + 4;
        continue;
      }

      MobiStructuredTocEntry e;
      e.title = title;
      e.pos = filepos;
      e.level = 0;
      raw_entries.push_back(e);

      pos = close + 4;
      if (raw_entries.size() >= 2048)
        break;
    }
  };

  scan_for_entries(first_pass_limit);
  size_t used_scan_limit = first_pass_limit;
  if (raw_entries.size() < 8 && first_pass_limit < full_scan_limit) {
    raw_entries.clear();
    scan_for_entries(full_scan_limit);
    used_scan_limit = full_scan_limit;
  }

  if (raw_entries.size() < 2)
    return false;

  // Merge duplicates by filepos and keep the best title for each location.
  std::map<u32, MobiStructuredTocEntry> by_pos;
  for (size_t i = 0; i < raw_entries.size(); i++) {
    const MobiStructuredTocEntry &e = raw_entries[i];
    auto it = by_pos.find(e.pos);
    if (it == by_pos.end()) {
      by_pos[e.pos] = e;
      continue;
    }
    int cur_score = ScoreMobiTocTitle(it->second.title);
    int new_score = ScoreMobiTocTitle(e.title);
    if (new_score > cur_score ||
        (new_score == cur_score && e.title.size() > it->second.title.size())) {
      it->second = e;
    }
  }

  size_t structured_like = 0;
  size_t low_quality = 0;
  for (const auto &kv : by_pos) {
    const MobiStructuredTocEntry &e = kv.second;
    if (LooksLikeStructuredMobiChapterTitle(e.title))
      structured_like++;
    if (ScoreMobiTocTitle(e.title) < 0)
      low_quality++;
    if (!out->empty() && out->back().pos == e.pos && out->back().title == e.title)
      continue;
    out->push_back(e);
  }

  if (out->size() < 2) {
    out->clear();
    return false;
  }

  // Quality gate: avoid replacing heuristic TOC with tiny/noisy filepos sets.
  size_t min_entries = (text_len > 1000000u) ? 8u : 4u;
  bool weak_size = out->size() < min_entries;
  bool weak_structure =
      (out->size() >= 4 && structured_like * 100 < out->size() * 30);
  bool weak_quality = (out->size() >= 4 && low_quality * 100 > out->size() * 55);
  if (weak_size || weak_structure || weak_quality) {
    if (app) {
      char msg[220];
      snprintf(msg, sizeof(msg),
               "MOBI: filepos TOC rejected kept=%u structured=%u lowq=%u min=%u",
               (unsigned)out->size(), (unsigned)structured_like,
               (unsigned)low_quality, (unsigned)min_entries);
      app->PrintStatus(msg);
    }
    out->clear();
    return false;
  }

  if (app) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "MOBI: filepos TOC entries raw=%u kept=%u structured=%u scan=%uKB",
             (unsigned)raw_entries.size(), (unsigned)out->size(),
             (unsigned)structured_like, (unsigned)(used_scan_limit / 1024));
    app->PrintStatus(msg);
  }
  return true;
}

static std::string ExtractMobiMarkupToText(
    const std::string &in, std::vector<MobiHeadingHint> *heading_hints) {
  std::string out;
  out.reserve(in.size());
  bool in_script = false;
  bool in_style = false;
  bool pending_space = false;
  int heading_level = -1;
  std::string heading_text;

  for (size_t i = 0; i < in.size();) {
    unsigned char c = (unsigned char)in[i];
    if (c == '<') {
      size_t close = in.find('>', i + 1);
      if (close == std::string::npos) {
        i++;
        continue;
      }

      std::string tag = TrimAsciiWhitespace(in.substr(i + 1, close - i - 1));
      i = close + 1;
      if (tag.empty())
        continue;
      if (tag.size() >= 3 && tag[0] == '!' && tag[1] == '-' && tag[2] == '-')
        continue;

      bool closing = false;
      if (!tag.empty() && tag[0] == '/') {
        closing = true;
        tag = TrimAsciiWhitespace(tag.substr(1));
      }

      std::string lower;
      lower.reserve(tag.size());
      for (size_t j = 0; j < tag.size(); j++) {
        char tc = tag[j];
        if (isspace((unsigned char)tc) || tc == '/')
          break;
        lower.push_back((char)tolower((unsigned char)tc));
      }

      if (lower.empty())
        continue;
      if (lower == "script") {
        in_script = !closing;
        continue;
      }
      if (lower == "style") {
        in_style = !closing;
        continue;
      }
      if (in_script || in_style)
        continue;

      int tag_heading_level = MobiHeadingTagLevel(lower);
      if (!closing && tag_heading_level >= 0) {
        heading_level = tag_heading_level;
        heading_text.clear();
      } else if (closing && tag_heading_level >= 0 && heading_level >= 0) {
        std::string normalized =
            CollapseAsciiWhitespace(TrimAsciiWhitespace(heading_text));
        if (heading_hints && normalized.size() >= 3 && normalized.size() <= 180) {
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

      if (lower == "br") {
        out.push_back('\n');
        if (heading_level >= 0 && !heading_text.empty() &&
            heading_text.back() != ' ')
          heading_text.push_back(' ');
        pending_space = false;
        continue;
      }
      if (lower == "li" && !closing) {
        AppendParagraphBreak(&out);
        out.append("- ");
        pending_space = false;
        continue;
      }
      if (IsMobiBlockTag(lower)) {
        AppendParagraphBreak(&out);
        pending_space = false;
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
          if (heading_level >= 0)
            heading_text += decoded;
          pending_space = false;
          i = semi + 1;
          continue;
        }
      }
    }

    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
      pending_space = true;
      i++;
      continue;
    }
    if (c < 0x20) {
      i++;
      continue;
    }

    if (pending_space) {
      AppendSingleSpace(&out);
      if (heading_level >= 0 && !heading_text.empty() &&
          heading_text.back() != ' ')
        heading_text.push_back(' ');
      pending_space = false;
    }

    if (c < 0x80) {
      out.push_back((char)c);
      if (heading_level >= 0)
        heading_text.push_back((char)c);
      i++;
      continue;
    }

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
    if (heading_level >= 0)
      heading_text.append(in, i, (size_t)step);
    i += (size_t)step;
  }

  return out;
}

static u8 ParseMobiFile(Book *book, const char *path) {
  if (!book || !path)
    return 251;
  App *app = book->GetApp();
  const u64 t_parse_begin = osGetTime();
  if (app)
    app->PrintStatus("MOBI: parse begin");

  g_mobi_deferred_states.erase(book);

  if (TryLoadMobiPageCache(book, path, app)) {
    if (app) {
      char msg[224];
      snprintf(msg, sizeof(msg),
               "MOBI: page cache hit pages=%u chapters=%u",
               (unsigned)book->GetPageCount(),
               (unsigned)book->GetChapters().size());
      app->PrintStatus(msg);

      char tmsg[160];
      snprintf(tmsg, sizeof(tmsg), "MOBI: timing cache_total=%llums",
               (unsigned long long)(osGetTime() - t_parse_begin));
      app->PrintStatus(tmsg);
      app->PrintStatus("MOBI: parse end");
    }
    return 0;
  }

  std::string raw;
  if (!ReadFileToStringLimited(path, &raw, kMobiMaxBytes))
    return 252;
  const u64 t_after_read = osGetTime();

  std::vector<u32> offsets;
  if (!ParseMobiOffsets(raw, &offsets) || offsets.size() < 3)
    return 253;

  const u8 *data = (const u8 *)raw.data();
  const u32 rec0_start = offsets[0];
  const u32 rec0_end = offsets[1];
  if (rec0_end <= rec0_start || rec0_end - rec0_start < 16)
    return 254;
  const u8 *rec0 = data + rec0_start;
  const size_t rec0_len = (size_t)(rec0_end - rec0_start);

  const u16 compression = ReadBE16(rec0 + 0);
  const u32 text_len = ReadBE32(rec0 + 4);
  u32 text_rec_count = ReadBE16(rec0 + 8);

  u32 encoding = 1252;
  u32 first_non_book_index = 0;
  u32 mobi_full_name_off = 0;
  u32 mobi_full_name_len = 0;
  u32 ncx_index = kMobiNullIndex;
  if (rec0_len >= 24 && memcmp(rec0 + 16, "MOBI", 4) == 0) {
    const u8 *mobi = rec0 + 16;
    const u32 mobi_len = ReadBE32(mobi + 4);
    if (mobi_len >= 0x20 && rec0_len >= 16 + (size_t)0x20)
      encoding = ReadBE32(mobi + 0x1C);
    if (mobi_len >= 0x84 && rec0_len >= 16 + (size_t)0x84)
      first_non_book_index = ReadBE32(mobi + 0x80);
    if (mobi_len >= 0x5C && rec0_len >= 16 + (size_t)0x5C) {
      mobi_full_name_off = ReadBE32(mobi + 0x54);
      mobi_full_name_len = ReadBE32(mobi + 0x58);
    }
    if (mobi_len >= 0xF8 && rec0_len >= 16 + (size_t)0xF8)
      ncx_index = ReadBE32(mobi + 0xF4);
  }

  if (app) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "MOBI: header comp=%u enc=%u text_len=%u text_recs=%u first_non_book=%u ncx=%u",
             (unsigned)compression, (unsigned)encoding, (unsigned)text_len,
             (unsigned)text_rec_count, (unsigned)first_non_book_index,
             (unsigned)ncx_index);
    app->PrintStatus(msg);
  }

  if (compression != 1 && compression != 2) {
    if (app) {
      if (compression == 17480)
        app->PrintStatus("MOBI: unsupported compression (HUFF/CDIC)");
      else
        app->PrintStatus("MOBI: unsupported compression");
    }
    return 255;
  }

  u32 max_text_records = (u32)offsets.size() - 2;
  if (text_rec_count == 0 || text_rec_count > max_text_records)
    text_rec_count = max_text_records;
  if (first_non_book_index > 1) {
    u32 boundary = first_non_book_index - 1;
    if (boundary > 0 && boundary < text_rec_count)
      text_rec_count = boundary;
  }
  if (text_rec_count == 0)
    return 255;

  if (mobi_full_name_len > 0 && mobi_full_name_len < 2048) {
    std::string title_raw;
    bool title_ok = false;
    if ((size_t)mobi_full_name_off + (size_t)mobi_full_name_len <= rec0_len) {
      title_raw.assign((const char *)rec0 + mobi_full_name_off,
                       (size_t)mobi_full_name_len);
      title_ok = true;
    } else if ((size_t)16 + (size_t)mobi_full_name_off +
                       (size_t)mobi_full_name_len <=
               rec0_len) {
      title_raw.assign((const char *)rec0 + 16 + (size_t)mobi_full_name_off,
                       (size_t)mobi_full_name_len);
      title_ok = true;
    }
    if (title_ok) {
      std::string title_utf8 = DecodeMobiBytesToUtf8(title_raw, encoding, NULL, NULL);
      title_utf8 = CollapseAsciiWhitespace(TrimAsciiWhitespace(title_utf8));
      if (!title_utf8.empty() && title_utf8.size() < 220)
        book->SetTitle(title_utf8.c_str());
    }
  }

  std::string merged;
  merged.reserve(text_len > 0 ? (size_t)text_len : 1024 * 1024);
  for (u32 rec = 1; rec <= text_rec_count; rec++) {
    u32 start = offsets[(size_t)rec];
    u32 end = offsets[(size_t)rec + 1];
    if (end <= start || end > raw.size())
      continue;
    const u8 *chunk = data + start;
    size_t chunk_len = (size_t)(end - start);

    if (compression == 1) {
      merged.append((const char *)chunk, chunk_len);
    } else {
      std::string decomp;
      if (DecompressPalmDocRecord(chunk, chunk_len, &decomp))
        merged += decomp;
    }
  }
  const u64 t_after_decompress = osGetTime();

  if (text_len > 0 && merged.size() > text_len)
    merged.resize((size_t)text_len);

  bool used_utf8_guess = false;
  bool used_legacy_guess = false;
  std::string utf8 =
      DecodeMobiBytesToUtf8(merged, encoding, &used_utf8_guess, &used_legacy_guess);
  const u64 t_after_decode = osGetTime();

  std::vector<MobiHeadingHint> heading_hints;
  std::string text = ExtractMobiMarkupToText(utf8, &heading_hints);
  NormalizeNewlines(&text);
  const u64 t_after_markup = osGetTime();

  std::vector<MobiStructuredTocEntry> structured_toc;
  bool structured_from_filepos = false;
  bool have_structured_toc = ParseMobiStructuredToc(
      raw, offsets, ncx_index, encoding, &structured_toc, app);
  if (!have_structured_toc &&
      ParseMobiInlineFileposToc(
          utf8, (text_len > 0) ? text_len : (u32)merged.size(), &structured_toc,
          app)) {
    have_structured_toc = true;
    structured_from_filepos = true;
  }

  MobiDeferredState deferred;
  deferred.source_path = path;
  deferred.text_utf8.swap(text);
  deferred.heading_hints.swap(heading_hints);
  deferred.structured_toc.swap(structured_toc);
  deferred.have_structured_toc = have_structured_toc;
  deferred.structured_from_filepos = structured_from_filepos;
  deferred.used_utf8_guess = used_utf8_guess;
  deferred.used_legacy_guess = used_legacy_guess;
  deferred.finalized = false;
  deferred.text_len_for_pos = (text_len > 0) ? text_len : (u32)merged.size();
  deferred.t_parse_begin = t_parse_begin;
  deferred.t_after_read = t_after_read;
  deferred.t_after_decompress = t_after_decompress;
  deferred.t_after_decode = t_after_decode;
  deferred.t_after_markup = t_after_markup;
  deferred.t_after_pages = 0;
  deferred.t_after_toc = 0;

  if (!InitPlainTextStreamState(book, deferred.text_utf8, false,
                                &deferred.stream)) {
    return 1;
  }

  const bool pages_done_initial =
      ContinuePlainTextStreamState(&deferred.stream, deferred.text_utf8, 1200, 96, 1);
  deferred.t_after_pages = osGetTime();

  if (!pages_done_initial) {
    g_mobi_deferred_states[book] = std::move(deferred);
    if (app) {
      char msg[224];
      snprintf(msg, sizeof(msg),
               "MOBI: deferred pagination pending pages=%u text_bytes=%u",
               (unsigned)book->GetPageCount(),
               (unsigned)g_mobi_deferred_states[book].text_utf8.size());
      app->PrintStatus(msg);

      char tmsg[320];
      snprintf(
          tmsg, sizeof(tmsg),
          "MOBI: timing read=%llums decomp=%llums decode=%llums markup=%llums initial=%llums total_open=%llums",
          (unsigned long long)(t_after_read - t_parse_begin),
          (unsigned long long)(t_after_decompress - t_after_read),
          (unsigned long long)(t_after_decode - t_after_decompress),
          (unsigned long long)(t_after_markup - t_after_decode),
          (unsigned long long)(deferred.t_after_pages - t_after_markup),
          (unsigned long long)(deferred.t_after_pages - t_parse_begin));
      app->PrintStatus(tmsg);
      app->PrintStatus("MOBI: parse end");
    }
    return 0;
  }

  MobiTocFinalizeResult toc_result;
  FinalizeMobiPreparedToc(book, app, deferred.structured_toc, deferred.have_structured_toc,
                          deferred.structured_from_filepos, deferred.heading_hints,
                          deferred.text_len_for_pos, &toc_result);
  deferred.t_after_toc = osGetTime();

  if (app) {
    char msg[320];
    snprintf(msg, sizeof(msg),
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
    app->PrintStatus(msg);

    char tmsg[320];
    snprintf(
        tmsg, sizeof(tmsg),
        "MOBI: timing read=%llums decomp=%llums decode=%llums markup=%llums pages=%llums toc=%llums total=%llums",
        (unsigned long long)(deferred.t_after_read - deferred.t_parse_begin),
        (unsigned long long)(deferred.t_after_decompress - deferred.t_after_read),
        (unsigned long long)(deferred.t_after_decode - deferred.t_after_decompress),
        (unsigned long long)(deferred.t_after_markup - deferred.t_after_decode),
        (unsigned long long)(deferred.t_after_pages - deferred.t_after_markup),
        (unsigned long long)(deferred.t_after_toc - deferred.t_after_pages),
        (unsigned long long)(deferred.t_after_toc - deferred.t_parse_begin));
    app->PrintStatus(tmsg);
  }

  SaveMobiPageCache(book, path, app);
  if (app)
    app->PrintStatus("MOBI: parse end");
  return 0;
}

static const char *XmlLocalName(const char *name) {
  if (!name)
    return "";
  const char *colon = strrchr(name, ':');
  return colon ? (colon + 1) : name;
}

static bool XmlAttrLocalNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle))
    return true;
  const char *local = XmlLocalName(name);
  return !strcmp(local, needle);
}

static bool OdtIsParagraphTag(const char *local_name) {
  if (!local_name)
    return false;
  return !strcmp(local_name, "p") || !strcmp(local_name, "h") ||
         !strcmp(local_name, "list-item") || !strcmp(local_name, "table-row");
}

struct OdtParseState {
  parsedata_t *parsedata;
  Book *book;
  int depth;
  int office_text_depth;
  int heading_depth;
  int heading_level;
  std::string heading_text;
  bool pending_space;
};

static bool ParsedataEndsWhitespace(parsedata_t *p) {
  if (!p || p->buflen <= 0)
    return true;
  unsigned char c = p->buf[p->buflen - 1];
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

static void OdtEmit(OdtParseState *s, const char *txt, int len) {
  if (!s || !s->parsedata || !txt || len <= 0)
    return;
  xml::book::chardata(s->parsedata, txt, len);
}

static void OdtEmitParagraphBreak(OdtParseState *s) {
  if (!s)
    return;
  OdtEmit(s, "\n\n", 2);
  s->pending_space = false;
}

static void odt_start(void *userdata, const char *el, const char **attr) {
  OdtParseState *s = (OdtParseState *)userdata;
  if (!s)
    return;
  s->depth++;

  if (!strcmp(el, "office:text")) {
    s->office_text_depth = s->depth;
    s->pending_space = false;
    return;
  }

  if (s->office_text_depth <= 0)
    return;

  const char *local = XmlLocalName(el);
  if (!strcmp(local, "line-break")) {
    OdtEmit(s, "\n", 1);
    s->pending_space = false;
    return;
  }
  if (!strcmp(local, "tab")) {
    OdtEmit(s, "\t", 1);
    s->pending_space = false;
    return;
  }
  if (!strcmp(local, "s")) {
    int count = 1;
    for (int i = 0; attr && attr[i]; i += 2) {
      if (!XmlAttrLocalNameEquals(attr[i], "c"))
        continue;
      int parsed = atoi(attr[i + 1] ? attr[i + 1] : "1");
      if (parsed > 0 && parsed < 64)
        count = parsed;
      break;
    }
    std::string spaces((size_t)count, ' ');
    OdtEmit(s, spaces.c_str(), (int)spaces.size());
    s->pending_space = false;
    return;
  }
  if (!strcmp(local, "image")) {
    OdtEmit(s, "\n[illustration]\n", 15);
    s->pending_space = false;
    return;
  }
  if (!strcmp(local, "h")) {
    s->heading_depth++;
    s->heading_level = 0;
    s->heading_text.clear();
    for (int i = 0; attr && attr[i]; i += 2) {
      if (!XmlAttrLocalNameEquals(attr[i], "outline-level"))
        continue;
      int level = atoi(attr[i + 1] ? attr[i + 1] : "1");
      if (level < 1)
        level = 1;
      if (level > 7)
        level = 7;
      s->heading_level = level - 1;
      break;
    }
  }
}

static void odt_chardata(void *userdata, const char *txt, int txtlen) {
  OdtParseState *s = (OdtParseState *)userdata;
  if (!s || s->office_text_depth <= 0 || !txt || txtlen <= 0)
    return;

  std::string out;
  out.reserve((size_t)txtlen + 1);
  for (int i = 0; i < txtlen; i++) {
    unsigned char c = (unsigned char)txt[i];
    bool is_space = (c == ' ' || c == '\n' || c == '\r' || c == '\t');
    if (is_space) {
      s->pending_space = true;
      continue;
    }
    if (s->pending_space) {
      if (!ParsedataEndsWhitespace(s->parsedata) || !out.empty())
        out.push_back(' ');
      s->pending_space = false;
    }
    out.push_back((char)c);
  }
  if (!out.empty())
    OdtEmit(s, out.c_str(), (int)out.size());
  if (s->heading_depth > 0 && !out.empty()) {
    if (!s->heading_text.empty() && s->heading_text.back() != ' ')
      s->heading_text.push_back(' ');
    s->heading_text += out;
  }
}

static void odt_end(void *userdata, const char *el) {
  OdtParseState *s = (OdtParseState *)userdata;
  if (!s)
    return;

  const char *local = XmlLocalName(el);
  if (s->office_text_depth > 0 && !strcmp(local, "h") && s->heading_depth > 0) {
    s->heading_depth--;
    if (s->heading_depth == 0 && s->book) {
      AddChapterIfUnique(s->book, s->heading_text, (u8)s->heading_level);
      s->heading_text.clear();
      s->heading_level = 0;
    }
  }
  if (s->office_text_depth > 0 && OdtIsParagraphTag(local)) {
    OdtEmitParagraphBreak(s);
  }

  if (s->office_text_depth > 0 && s->depth == s->office_text_depth &&
      !strcmp(el, "office:text")) {
    s->office_text_depth = 0;
    s->pending_space = false;
  }
  s->depth--;
}

static bool ReadZipEntryToStringLimited(unzFile uf, const char *entry_name,
                                        std::string *out, size_t max_bytes) {
  if (!uf || !entry_name || !out)
    return false;
  out->clear();

  if (unzLocateFile(uf, entry_name, 2) != UNZ_OK)
    return false;
  if (unzOpenCurrentFile(uf) != UNZ_OK)
    return false;

  char buf[8192];
  int n = 0;
  bool ok = true;
  while ((n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) {
    if (out->size() + (size_t)n > max_bytes) {
      ok = false;
      break;
    }
    out->append(buf, n);
  }
  if (n < 0)
    ok = false;
  unzCloseCurrentFile(uf);
  return ok;
}

static u8 ParseOdtFile(Book *book, const char *path) {
  if (!book || !path)
    return 251;

  unzFile uf = unzOpen(path);
  if (!uf)
    return 252;

  std::string content_xml;
  bool loaded = ReadZipEntryToStringLimited(uf, "content.xml", &content_xml,
                                            kOdtContentMaxBytes);
  unzClose(uf);
  if (!loaded || content_xml.empty())
    return 253;

  parsedata_t parsedata;
  parse_init(&parsedata);
  parsedata.app = book->GetApp();
  parsedata.ts = parsedata.app ? parsedata.app->ts : NULL;
  parsedata.prefs = parsedata.app ? parsedata.app->prefs : NULL;
  parsedata.book = book;
  parse_push(&parsedata, TAG_PRE);
  book->ClearChapters();
  book->ClearTocConfidence();

  XML_Parser p = XML_ParserCreate(NULL);
  if (!p) {
    parse_pop(&parsedata);
    return 254;
  }

  OdtParseState odt_state;
  odt_state.parsedata = &parsedata;
  odt_state.book = book;
  odt_state.depth = 0;
  odt_state.office_text_depth = 0;
  odt_state.heading_depth = 0;
  odt_state.heading_level = 0;
  odt_state.heading_text.clear();
  odt_state.pending_space = false;

  XML_SetUserData(p, &odt_state);
  XML_SetElementHandler(p, odt_start, odt_end);
  XML_SetCharacterDataHandler(p, odt_chardata);

  bool ok = XML_Parse(p, content_xml.c_str(), (int)content_xml.size(), 1) !=
            XML_STATUS_ERROR;
  if (!ok && parsedata.app) {
    parsedata.app->parse_error(p);
  }

  XML_ParserFree(p);
  parse_pop(&parsedata);

  if (!ok)
    return 255;

  FinalizePlainPage(&parsedata);
  SetNonEpubTocConfidence(book, true);
  return 0;
}

static bool FinalizeDeferredMobiState(Book *book, MobiDeferredState *state) {
  if (!book || !state)
    return true;
  if (state->finalized)
    return true;
  if (!state->stream.completed)
    return false;

  App *app = book->GetApp();
  MobiTocFinalizeResult toc_result;
  FinalizeMobiPreparedToc(book, app, state->structured_toc, state->have_structured_toc,
                          state->structured_from_filepos, state->heading_hints,
                          state->text_len_for_pos, &toc_result);
  state->t_after_toc = osGetTime();
  state->finalized = true;

  if (app) {
    char msg[320];
    snprintf(msg, sizeof(msg),
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
    app->PrintStatus(msg);

    char tmsg[320];
    snprintf(
        tmsg, sizeof(tmsg),
        "MOBI: timing read=%llums decomp=%llums decode=%llums markup=%llums pages=%llums toc=%llums total=%llums",
        (unsigned long long)(state->t_after_read - state->t_parse_begin),
        (unsigned long long)(state->t_after_decompress - state->t_after_read),
        (unsigned long long)(state->t_after_decode - state->t_after_decompress),
        (unsigned long long)(state->t_after_markup - state->t_after_decode),
        (unsigned long long)(state->t_after_pages - state->t_after_markup),
        (unsigned long long)(state->t_after_toc - state->t_after_pages),
        (unsigned long long)(state->t_after_toc - state->t_parse_begin));
    app->PrintStatus(tmsg);

    char dmsg[160];
    snprintf(dmsg, sizeof(dmsg),
             "MOBI: deferred pagination complete pages=%u chapters=%u",
             (unsigned)book->GetPageCount(),
             (unsigned)book->GetChapters().size());
    app->PrintStatus(dmsg);
  }

  SaveMobiPageCache(book, state->source_path.c_str(), app);
  return true;
}

static bool ContinueDeferredMobiState(Book *book, MobiDeferredState *state,
                                      u32 budget_ms, u16 page_budget) {
  if (!book || !state)
    return true;

  const u16 pages_before = book->GetPageCount();
  const bool done = ContinuePlainTextStreamState(&state->stream, state->text_utf8,
                                                 budget_ms, page_budget, 0);
  if (book->GetPageCount() > pages_before)
    state->t_after_pages = osGetTime();

  if (!done)
    return false;
  return FinalizeDeferredMobiState(book, state);
}

} // namespace

void Book::Cache() {
  FILE *fp = fopen("/cache.dat", "w");
  if (!fp)
    return;

  int buflen = 0;
  int pagecount = GetPageCount();
  fprintf(fp, "%d\n", pagecount);
  for (int i = 0; i < pagecount; i++) {
    buflen += GetPage(i)->GetLength();
    fprintf(fp, "%d\n", buflen);
    GetPage(i)->Cache(fp);
  }
  fclose(fp);
}

u8 Book::Open() {
  std::string path;
  path.append(GetFolderName());
  path.append("/");
  path.append(GetFileName());

  char logmsg[256];
  sprintf(logmsg, "Opening: %s", path.c_str());
  app->PrintStatus(logmsg);

  // Page layout is a function of the current style.
  app->ts->SetStyle(TEXT_STYLE_REGULAR);
  tocResolveTried = false;
  tocResolved = false;
  ClearTocConfidence();
  ClearChapterAnchors();
  u8 err = 1;
  if (format == FORMAT_EPUB) {
    err = epub(this, path, false);
  } else {
    err = Parse(true);
  }
  if (!err)
    if (position > (int)pages.size())
      position = pages.size() - 1;
  return err;
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
    return ParseOdtFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".mobi"))
    return ParseMobiFile(this, path);

  char *filebuf = new char[BUFSIZE];
  if (!filebuf) {
    rc = 1;
    return (rc);
  }

  FILE *fp = fopen(path, "r");
  if (!fp) {
    delete[] filebuf;
    rc = 255;
    return (rc);
  }

  parsedata_t parsedata;
  parse_init(&parsedata);
  parsedata.fb2_mode = fulltext && HasExtCI(GetFileName(), ".fb2");
  parsedata.cachefile = fopen("/cache.dat", "w");
  parsedata.app = app;
  parsedata.ts = app ? app->ts : NULL;
  parsedata.prefs = app ? app->prefs : NULL;
  parsedata.book = this;

  XML_Parser p = XML_ParserCreate(NULL);
  if (!p) {
    delete[] filebuf;
    fclose(fp);
    rc = 253;
    return rc;
  }
  XML_ParserReset(p, NULL);
  XML_SetUserData(p, &parsedata);
  XML_SetDefaultHandler(p, xml::book::fallback);
  XML_SetProcessingInstructionHandler(p, xml::book::instruction);
  XML_SetElementHandler(p, xml::book::start, xml::book::end);
  XML_SetCharacterDataHandler(p, xml::book::chardata);
  if (!fulltext) {
    XML_SetElementHandler(p, xml::book::metadata::start,
                          xml::book::metadata::end);
    XML_SetCharacterDataHandler(p, xml::book::metadata::chardata);
  }

  enum XML_Status status;
  while (true) {
    int bytes_read = fread(filebuf, 1, BUFSIZE, fp);
    status = XML_Parse(p, filebuf, bytes_read, (bytes_read == 0));
    if (status == XML_STATUS_ERROR) {
      app->parse_error(p);
      rc = 254;
      break;
    }
    if (parsedata.status)
      break; // non-fulltext parsing signals it is done.
    if (bytes_read == 0)
      break; // assume our buffer ran out.
  }

  XML_ParserFree(p);
  fclose(fp);
  delete[] filebuf;

  if (rc == 0 && fulltext && parsedata.fb2_mode) {
    bool has_structured_toc = !chapters.empty();
    if (!has_structured_toc)
      BuildFb2FallbackChapters(this);
    if (!chapters.empty())
      SetNonEpubTocConfidence(this, has_structured_toc);
    else
      ClearTocConfidence();
  }

  return (rc);
}

void Book::Restore() {
  FILE *fp = fopen("/cache.dat", "r");
  if (!fp)
    return;

  int len, pagecount;
  u8 buf[BUFSIZE];

  fscanf(fp, "%d\n", &pagecount);
  for (int i = 0; i < pagecount - 1; i++) {
    fscanf(fp, "%d\n", &len);
    fread(buf, sizeof(char), len, fp);
    GetPage(i)->SetBuffer(buf, len);
  }
  fclose(fp);
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

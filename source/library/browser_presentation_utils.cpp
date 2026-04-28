#include "library/browser_presentation_utils.h"

#include <vector>

#include <set>

#include "book/book.h"
#include "shared/debug_log.h"
#include "shared/string_utils.h"
#include "shared/utf8_utils.h"
#include "ui/text.h"

#ifndef UTF8_FILENAME_DIAG
#define UTF8_FILENAME_DIAG 0
#endif

namespace {

static std::string TrimSpacesLocal(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && s[start] == ' ')
    start++;
  size_t end = s.size();
  while (end > start && s[end - 1] == ' ')
    end--;
  return s.substr(start, end - start);
}

#if UTF8_FILENAME_DIAG
static std::string HexBytesForLog(const std::string &s, size_t max_bytes = 32) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  size_t n = s.size() < max_bytes ? s.size() : max_bytes;
  out.reserve(n * 3 + 8);
  for (size_t i = 0; i < n; i++) {
    unsigned char b = (unsigned char)s[i];
    if (i)
      out.push_back(' ');
    out.push_back(hex[(b >> 4) & 0x0F]);
    out.push_back(hex[b & 0x0F]);
  }
  if (s.size() > max_bytes)
    out += " ...";
  return out;
}

static std::string ClipForLog(const std::string &s, size_t max_chars = 72) {
  if (s.size() <= max_chars)
    return s;
  return s.substr(0, max_chars) + "...";
}
#endif

static void LogUtf8StageOnce(Book *book, const char *stage,
                             const std::string &value) {
#if !UTF8_FILENAME_DIAG
  (void)book;
  (void)stage;
  (void)value;
  return;
#else
  if (!book || !book->GetStatusReporter() || !stage)
    return;

  static std::set<std::string> logged;
  char key[96];
  snprintf(key, sizeof(key), "%p|%s", (void *)book, stage);
  if (!logged.insert(std::string(key)).second)
    return;

  char msg[512];
  std::string bytes = HexBytesForLog(value);
  std::string clipped = ClipForLog(value);
  snprintf(msg, sizeof(msg),
           "UTF8 flow %-18s len=%u valid=%d bytes=[%s] text=\"%s\"", stage,
           (unsigned)value.size(), utf8_utils::IsValidUtf8(value) ? 1 : 0,
           bytes.c_str(), clipped.c_str());
  DBG_LOG(book->GetStatusReporter(), msg);
#endif
}

static std::string NormalizeDisplayUtf8(const std::string &raw,
                                        bool *repaired_fullwidth = NULL,
                                        bool *repaired_legacy = NULL,
                                        bool *composed_accents = NULL) {
  if (repaired_fullwidth)
    *repaired_fullwidth = false;
  if (repaired_legacy)
    *repaired_legacy = false;
  if (composed_accents)
    *composed_accents = false;

  if (raw.empty())
    return raw;

  std::string s = raw;
  std::string repaired;
  if (utf8_utils::TryRepairFullwidthByteMojibake(s, &repaired)) {
    s = repaired;
    if (repaired_fullwidth)
      *repaired_fullwidth = true;
  }

  if (!utf8_utils::IsValidUtf8(s)) {
    if (repaired_legacy)
      *repaired_legacy = true;
    return utf8_utils::DecodeCp1252ToUtf8(s);
  }

  if (utf8_utils::TryRepairMojibakeUtf8(s, &repaired)) {
    if (repaired_legacy)
      *repaired_legacy = true;
    s = repaired;
  }

  std::string composed = utf8_utils::ComposeLatinCombiningMarks(s);
  if (composed != s) {
    s = composed;
    if (composed_accents)
      *composed_accents = true;
  }
  return s;
}

} // namespace

namespace browser_presentation_utils {

std::string BuildBrowserDisplayName(Book *book) {
  if (!book)
    return "";
  if (book->IsBrowserFolder()) {
    std::string name = book->GetBrowserFolderDisplayName();
    if (!name.empty() && name[name.size() - 1] != '/')
      name.push_back('/');
    return name;
  }
  if (book->HasBrowserDisplayNameCache())
    return book->GetBrowserDisplayNameCache();

  const char *filename_ptr = book->GetFileName();
  const char *source =
      BrowserDisplayNameSource(book->GetTitle(), filename_ptr);
  bool source_is_filename = (source == filename_ptr);
  std::string raw = source ? source : "";
  LogUtf8StageOnce(book, "filename_raw", raw);

  bool repaired_fullwidth = false;
  bool repaired_legacy = false;
  bool composed_accents = false;
  std::string normalized = NormalizeDisplayUtf8(
      raw, &repaired_fullwidth, &repaired_legacy, &composed_accents);
  if (repaired_fullwidth)
    LogUtf8StageOnce(book, "filename_ff_repair", normalized);
  if (repaired_legacy)
    LogUtf8StageOnce(book, "filename_legacy_fix", normalized);
  if (composed_accents)
    LogUtf8StageOnce(book, "filename_compose", normalized);
  LogUtf8StageOnce(book, "filename_norm", normalized);

  if (source_is_filename) {
    size_t dot = normalized.find_last_of('.');
    if (dot != std::string::npos)
      normalized = normalized.substr(0, dot);
  }

  normalized = TrimSpacesLocal(normalized);
  LogUtf8StageOnce(book, "filename_final", normalized);
  book->SetBrowserDisplayNameCache(normalized);
  return book->GetBrowserDisplayNameCache();
}

size_t Utf8BytesForCharCount(const char *s, size_t char_count) {
  if (!s)
    return 0;
  size_t bytes = 0;
  size_t chars = 0;
  while (s[bytes] && chars < char_count) {
    unsigned char c = (unsigned char)s[bytes];
    size_t step = 1;
    if ((c & 0xE0) == 0xC0)
      step = 2;
    else if ((c & 0xF0) == 0xE0)
      step = 3;
    else if ((c & 0xF8) == 0xF0)
      step = 4;

    for (size_t i = 1; i < step; i++) {
      if (!s[bytes + i]) {
        step = i;
        break;
      }
    }
    bytes += step;
    chars++;
  }
  return bytes;
}

void DrawWrappedTitleInsideCover(Text *ts, const std::string &title,
                                 int x, int y, int w, int h,
                                 unsigned char style) {
  if (!ts || title.empty() || w <= 8 || h <= 8)
    return;

  const int kPadX = 8;
  const int kPadY = 10;
  const int kMaxLines = 4;
  const int inner_w = w - kPadX * 2;
  const int inner_h = h - kPadY * 2;
  if (inner_w <= 8 || inner_h <= 8)
    return;

  int line_h = ts->GetHeight();
  int max_lines = inner_h / ((line_h > 1) ? line_h : 1);
  if (max_lines < 1)
    return;
  if (max_lines > kMaxLines)
    max_lines = kMaxLines;

  std::vector<std::string> lines;
  lines.reserve((size_t)max_lines);

  size_t pos = 0;
  while (pos < title.size() && (int)lines.size() < max_lines) {
    while (pos < title.size() && title[pos] == ' ')
      pos++;
    if (pos >= title.size())
      break;

    unsigned char fit =
        ts->GetCharCountInsideWidth(title.c_str() + pos, style, inner_w);
    if (!fit)
      break;

    size_t take = Utf8BytesForCharCount(title.c_str() + pos, fit);
    if (pos + take < title.size()) {
      size_t back = take;
      while (back > 0 && title[pos + back - 1] != ' ')
        back--;
      if (back > 0)
        take = back;
    }
    std::string line = TrimSpacesLocal(title.substr(pos, take));
    if (line.empty()) {
      pos += take;
      continue;
    }
    lines.push_back(line);
    pos += take;
  }

  if (lines.empty())
    return;

  const int saved_margin_left = ts->margin.left;
  const int saved_margin_right = ts->margin.right;
  const bool saved_clip = ts->IsClipToContentEnabled();
  const bool saved_wrap = ts->IsAutoWrapEnabled();

  ts->margin.left = x + kPadX;
  ts->margin.right =
      ts->display.width - std::min(ts->display.width, x + kPadX + inner_w);
  ts->SetClipToContentEnabled(true);
  ts->SetAutoWrapEnabled(false);

  const int text_h = (int)lines.size() * line_h;
  const int top_pad = kPadY + std::max(0, (inner_h - text_h) / 2);
  for (size_t i = 0; i < lines.size(); i++) {
    ts->SetPen(x + kPadX, y + top_pad + (int)(i + 1) * line_h);
    ts->PrintString(lines[i].c_str(), style);
  }

  ts->margin.left = saved_margin_left;
  ts->margin.right = saved_margin_right;
  ts->SetClipToContentEnabled(saved_clip);
  ts->SetAutoWrapEnabled(saved_wrap);
}

} // namespace browser_presentation_utils

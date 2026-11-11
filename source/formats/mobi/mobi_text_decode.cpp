/*
    3dslibris - mobi_text_decode.cpp
*/

#include "formats/mobi/mobi_text_decode.h"
#include "formats/common/cache_memory_utils.h"

#include <ctype.h>
#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include "book/book.h"
#include "formats/common/text_helpers.h"

namespace mobi_text_decode {

bool IsUtf16Encoding(u32 encoding) {
  return encoding == 1200 || encoding == 65002;
}

static bool IsUtf8Encoding(u32 encoding) {
  return encoding == 65001;
}

static bool IsUnknownEncoding(u32 encoding) {
  return encoding == 0 || encoding == 0xFFFFFFFFu;
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

  for (; i + 1 < in.size();) {
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
  size_t score = 0;
  score += CountNeedleOccurrences(utf8, "\xC3\x83");
  score += CountNeedleOccurrences(utf8, "\xC3\x82");
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x82\xAC");
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x80\x99");
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x80\x9C");
  score += CountNeedleOccurrences(utf8, "\xC3\xA2\xE2\x80\x9D");
  return score;
}

static bool LooksLikeMojibakeUtf8Text(const std::string &utf8) {
  return CountMojibakeMarkers(utf8) >= 2;
}

std::string DecodeBytesToUtf8(const std::string &in, u32 encoding,
                              bool *used_utf8_guess, bool *used_legacy_guess) {
  if (used_utf8_guess)
    *used_utf8_guess = false;
  if (used_legacy_guess)
    *used_legacy_guess = false;

  if (IsUtf16Encoding(encoding))
    return DecodeUtf16ToUtf8(in);

  const bool raw_is_utf8 = LooksLikeValidUtf8Bytes(in);
  const size_t invalid_utf8_leads =
      raw_is_utf8 ? 0 : CountUtf8InvalidLeadBytes(in);
  const bool mostly_utf8 =
      !in.empty() && (invalid_utf8_leads * 1000 <= in.size() * 2);
  if (IsUtf8Encoding(encoding))
    return raw_is_utf8 ? in : DecodeMostlyUtf8WithCp1252Fallback(in, NULL);

  if (IsUnknownEncoding(encoding)) {
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

struct Session::State {
  const std::string *source;
  std::string *output;
  u32 encoding;
  size_t cursor = 0;
  Scorer scorer;
  bool converting = false, done = false, guessed = false, legacy_guess = false,
       no_memory = false;
  typedef DecodeMode Mode;
  Mode mode = Legacy;
};
namespace {
size_t SafeEnd(const std::string &s, size_t from, size_t bytes) {
  size_t i = from, end = std::min(s.size(), from + std::max<size_t>(1, bytes));
  while (i < end) {
    const unsigned char c = s[i];
    const size_t n = (c & 0xe0) == 0xc0   ? 2
                     : (c & 0xf0) == 0xe0 ? 3
                     : (c & 0xf8) == 0xf0 ? 4
                                          : 1;
    bool valid = n > 1 && i + n <= s.size();
    for (size_t j = 1; valid && j < n; ++j)
      valid = ((unsigned char)s[i + j] & 0xc0) == 0x80;
    i += valid ? n : 1;
  }
  return i;
}
size_t ScoreChunk(std::string &tail, const std::string &chunk) {
  const std::string joined = tail + chunk;
  const size_t score =
      CountMojibakeMarkers(joined) - CountMojibakeMarkers(tail);
  tail = joined.substr(joined.size() > 8 ? joined.size() - 8 : 0);
  return score;
}
// Walk boundary the whole-buffer scan would use: how many leading bytes of the
// buffer form complete characters. With !final, a trailing sequence that is
// still a valid prefix is held back for the next feed.
size_t CompleteEnd(const char *p, size_t size, bool final) {
  size_t i = 0;
  while (i < size) {
    const unsigned char c = p[i];
    const size_t n = (c & 0xe0) == 0xc0   ? 2
                     : (c & 0xf0) == 0xe0 ? 3
                     : (c & 0xf8) == 0xf0 ? 4
                                          : 1;
    if (n == 1) {
      ++i;
      continue;
    }
    bool prefix = true;
    for (size_t j = 1; prefix && j < n && i + j < size; ++j)
      prefix = ((unsigned char)p[i + j] & 0xc0) == 0x80;
    if (prefix && i + n <= size) {
      i += n;
      continue;
    }
    if (prefix && !final)
      return i; // partial sequence: wait for more bytes
    ++i;
  }
  return i;
}
} // namespace

Scorer::Scorer() {}
void Scorer::Feed(const char *data, size_t n, bool final) {
  std::string work;
  work.swap(carry_);
  work.append(data, n);
  const size_t end = CompleteEnd(work.data(), work.size(), final);
  if (end < work.size())
    carry_.assign(work, end, std::string::npos);
  work.resize(end);
  score_.bytes += end;
  score_.invalid += CountUtf8InvalidLeadBytes(work);
  const std::string legacy = DecodeLegacySingleByteToUtf8(work);
  score_.legacy_size += legacy.size();
  score_.legacy_score += ScoreChunk(legacy_tail_, legacy);
  const std::string mixed = DecodeMostlyUtf8WithCp1252Fallback(work, NULL);
  score_.mixed_size += mixed.size();
  score_.mixed_score += ScoreChunk(mixed_tail_, mixed);
}

ModeChoice ChooseMode(u32 encoding, const ScanScore &score) {
  ModeChoice c = {Legacy, false, false};
  const bool valid = !score.invalid,
             mostly = score.bytes && score.invalid * 1000 <= score.bytes * 2;
  if (IsUtf8Encoding(encoding))
    c.mode = valid ? Copy : Mixed;
  else if (IsUnknownEncoding(encoding)) {
    c.mode = valid ? Copy : mostly ? Mixed : Legacy;
    c.guessed = valid || mostly;
    c.legacy_guess = !c.guessed;
  } else if ((valid || mostly) && score.legacy_score >= 2 &&
             (valid || score.mixed_score + 1 < score.legacy_score)) {
    c.mode = valid ? Copy : Mixed;
    c.guessed = true;
  }
  return c;
}

Converter::Converter(DecodeMode mode)
    : mode_(mode), started_(false), little_(true) {}
void Converter::Feed(const char *data, size_t n, bool final,
                     std::string *out) {
  if (mode_ == Copy) {
    out->append(data, n);
    return;
  }
  if (mode_ == Legacy) {
    *out += DecodeLegacySingleByteToUtf8(std::string(data, n));
    return;
  }
  std::string work;
  work.swap(carry_);
  work.append(data, n);
  if (mode_ == Mixed) {
    const size_t end = CompleteEnd(work.data(), work.size(), final);
    if (end < work.size())
      carry_.assign(work, end, std::string::npos);
    work.resize(end);
    *out += DecodeMostlyUtf8WithCp1252Fallback(work, NULL);
    return;
  }
  size_t i = 0;
  if (!started_) {
    if (work.size() < 2 && !final) {
      carry_.swap(work);
      return;
    }
    started_ = true;
    if (work.size() >= 2) {
      const unsigned char b0 = work[0], b1 = work[1];
      if (b0 == 0xfe && b1 == 0xff) {
        little_ = false;
        i = 2;
      } else if (b0 == 0xff && b1 == 0xfe)
        i = 2;
    }
  }
  auto word = [&](size_t at) {
    return little_ ? (u16)((u8)work[at] | ((u16)(u8)work[at + 1] << 8))
                   : (u16)(((u16)(u8)work[at] << 8) | (u8)work[at + 1]);
  };
  while (i + 1 < work.size()) {
    u32 cp = word(i);
    size_t used = 2;
    if (cp >= 0xd800 && cp <= 0xdbff) {
      if (i + 3 < work.size()) {
        const u16 next = word(i + 2);
        if (next >= 0xdc00 && next <= 0xdfff) {
          cp = 0x10000 + ((cp - 0xd800) << 10) + (next - 0xdc00);
          used = 4;
        }
      } else if (!final)
        break; // the low surrogate may be in the next feed
    }
    AppendUtf8Codepoint(out, cp);
    i += used;
  }
  if (!final && i < work.size())
    carry_.assign(work, i, std::string::npos);
}

Session::Session() : state_(NULL) {}
Session::~Session() {
  delete state_;
}
bool Session::Begin(const std::string *source, u32 encoding,
                    std::string *output) {
  delete state_;
  state_ = new (std::nothrow) State;
  if (!state_ || !source || !output)
    return false;
  State &s = *state_;
  s.source = source;
  s.output = output;
  s.encoding = encoding;
  output->clear();
  if (IsUtf16Encoding(encoding)) {
    s.mode = Utf16;
    s.converting = true;
  }
  return true;
}
size_t Session::Progress() const {
  return state_ ? state_->cursor +
                      (state_->converting ? state_->source->size() : 0)
                : 0;
}
size_t Session::Total() const {
  return state_ ? state_->source->size() * 2 : 0;
}
bool Session::NoMemory() const {
  return state_ && state_->no_memory;
}
bool Session::UsedUtf8Guess() const {
  return state_ && state_->guessed;
}
bool Session::UsedLegacyGuess() const {
  return state_ && state_->legacy_guess;
}
bool Session::Step(size_t bytes) {
  if (!state_ || state_->done)
    return true;
  State &s = *state_;
  const std::string &in = *s.source;
  if (!s.converting) {
    const size_t end = SafeEnd(in, s.cursor, bytes);
    s.scorer.Feed(in.data() + s.cursor, end - s.cursor, end == in.size());
    s.cursor = end;
    if (end < in.size())
      return false;
    const ModeChoice choice = ChooseMode(s.encoding, s.scorer.Score());
    s.mode = choice.mode;
    s.guessed = choice.guessed;
    s.legacy_guess = choice.legacy_guess;
    s.cursor = 0;
    s.converting = true;
    return false;
  }
  if (s.no_memory)
    return false;
  const size_t chunk =
      std::min(in.size() - s.cursor, std::max<size_t>(bytes, 4) + 4);
  if (!cache_memory::CanAppend(*s.output, chunk * 3)) {
    s.no_memory = true;
    return false;
  }
  if (s.mode == Utf16) {
    bool little = true;
    if (in.size() >= 2 && (unsigned char)in[0] == 0xfe &&
        (unsigned char)in[1] == 0xff)
      little = false;
    if (!s.cursor && in.size() >= 2 &&
        (((unsigned char)in[0] == 0xff && (unsigned char)in[1] == 0xfe) ||
         !little))
      s.cursor = 2;
    const size_t end =
        std::min(in.size(), s.cursor + std::max<size_t>(bytes, 2));
    auto word = [&](size_t i) {
      return little ? (u16)((u8)in[i] | ((u16)(u8)in[i + 1] << 8))
                    : (u16)(((u16)(u8)in[i] << 8) | (u8)in[i + 1]);
    };
    while (s.cursor < end && s.cursor + 1 < in.size()) {
      u32 cp = word(s.cursor);
      s.cursor += 2;
      if (cp >= 0xd800 && cp <= 0xdbff && s.cursor + 1 < in.size()) {
        const u16 next = word(s.cursor);
        if (next >= 0xdc00 && next <= 0xdfff) {
          cp = 0x10000 + ((cp - 0xd800) << 10) + (next - 0xdc00);
          s.cursor += 2;
        }
      }
      AppendUtf8Codepoint(s.output, cp);
    }
    if (s.cursor + 1 >= in.size())
      s.cursor = in.size();
  } else {
    const size_t end = SafeEnd(in, s.cursor, bytes);
    const std::string part = in.substr(s.cursor, end - s.cursor);
    if (s.mode == Copy)
      *s.output += part;
    else if (s.mode == Mixed)
      *s.output += DecodeMostlyUtf8WithCp1252Fallback(part, NULL);
    else
      *s.output += DecodeLegacySingleByteToUtf8(part);
    s.cursor = end;
  }
  return s.done = s.cursor == in.size();
}

void ApplyEmbeddedTitle(Book *book, const std::string &raw,
                        const mobi_parser_core::MobiHeaderInfo &header) {
  if (!book || header.mobi_full_name_len == 0 ||
      header.mobi_full_name_len >= 2048)
    return;

  const u8 *data = (const u8 *)raw.data();
  const u32 rec0_start = header.offsets.empty() ? 0 : header.offsets[0];
  const u32 rec0_end = (header.offsets.size() > 1) ? header.offsets[1] : 0;
  if (rec0_end <= rec0_start || rec0_end - rec0_start < 16)
    return;
  ApplyEmbeddedTitleFromRecord0(book, data + rec0_start,
                                (size_t)(rec0_end - rec0_start), header);
}

void ApplyEmbeddedTitleFromRecord0(
    Book *book, const u8 *rec0, size_t rec0_len,
    const mobi_parser_core::MobiHeaderInfo &header) {
  if (!book || !rec0 || rec0_len < 16 || header.mobi_full_name_len == 0 ||
      header.mobi_full_name_len >= 2048)
    return;

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
    title_raw.assign((const char *)rec0 + 16 +
                         (size_t)header.mobi_full_name_off,
                     (size_t)header.mobi_full_name_len);
    title_ok = true;
  }
  if (!title_ok)
    return;

  std::string title_utf8 =
      DecodeBytesToUtf8(title_raw, header.encoding, NULL, NULL);
  title_utf8 = CollapseAsciiWhitespace(TrimAsciiWhitespace(title_utf8));
  if (!title_utf8.empty() && title_utf8.size() < 220)
    book->SetTitle(title_utf8.c_str());
}

} // namespace mobi_text_decode

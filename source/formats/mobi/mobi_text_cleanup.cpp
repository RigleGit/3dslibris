#include "formats/mobi/mobi_text_cleanup.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace mobi_text_cleanup {

namespace {

// Keep these values in sync with include/text.h without pulling 3DS-specific
// headers into the host-side unit tests for this module.
static const unsigned char kTextImage = 6;
static const unsigned char kTextImageLeadingParagraph = 7;
static const unsigned char kTextImageFigureWithCaption = 8;
static const unsigned char kTextImageContextDefault = 14;

struct MobiTokenizedPiece {
  bool is_image_token;
  std::string bytes;
};

static bool IsMobiImageContextByte(unsigned char c) {
  return c == kTextImageLeadingParagraph ||
         c == kTextImageFigureWithCaption ||
         c == kTextImageContextDefault;
}

static size_t MobiImageTokenLengthAt(const std::string &text, size_t pos) {
  if (pos >= text.size())
    return 0;

  const unsigned char c0 = (unsigned char)text[pos];
  if (c0 == kTextImage) {
    return (pos + 2 < text.size()) ? 3u : 0u;
  }
  if (IsMobiImageContextByte(c0) && pos + 3 < text.size() &&
      (unsigned char)text[pos + 1] == kTextImage) {
    return 4u;
  }
  return 0;
}

static std::vector<MobiTokenizedPiece>
TokenizePreservingMobiImageTokens(const std::string &text) {
  std::vector<MobiTokenizedPiece> pieces;
  std::string current_text;
  current_text.reserve(text.size());

  for (size_t i = 0; i < text.size();) {
    const size_t token_len = MobiImageTokenLengthAt(text, i);
    if (token_len == 0) {
      current_text.push_back(text[i]);
      i++;
      continue;
    }

    if (!current_text.empty()) {
      MobiTokenizedPiece text_piece;
      text_piece.is_image_token = false;
      text_piece.bytes.swap(current_text);
      pieces.push_back(text_piece);
    }

    MobiTokenizedPiece token_piece;
    token_piece.is_image_token = true;
    token_piece.bytes = text.substr(i, token_len);
    pieces.push_back(token_piece);
    i += token_len;
  }

  if (!current_text.empty()) {
    MobiTokenizedPiece text_piece;
    text_piece.is_image_token = false;
    text_piece.bytes.swap(current_text);
    pieces.push_back(text_piece);
  }

  return pieces;
}

template <typename CleanupFn>
static std::string ApplyCleanupPreservingMobiImageTokens(
    const std::string &text, CleanupFn cleanup) {
  const std::vector<MobiTokenizedPiece> pieces =
      TokenizePreservingMobiImageTokens(text);
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < pieces.size(); i++) {
    if (pieces[i].is_image_token) {
      out += pieces[i].bytes;
    } else {
      const std::string &piece = pieces[i].bytes;
      size_t lead = 0;
      while (lead < piece.size() &&
             std::isspace((unsigned char)piece[lead]) != 0) {
        lead++;
      }
      size_t tail = piece.size();
      while (tail > lead &&
             std::isspace((unsigned char)piece[tail - 1]) != 0) {
        tail--;
      }
      out.append(piece, 0, lead);
      out += cleanup(piece.substr(lead, tail - lead));
      out.append(piece, tail, piece.size() - tail);
    }
  }
  return out;
}

static void AppendUtf8Codepoint(std::string *out, unsigned int cp) {
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

static bool DecodeLatin1Supplement(const std::string &text, size_t start,
                                   size_t *consumed, unsigned int *cp_out) {
  if (consumed)
    *consumed = 0;
  if (cp_out)
    *cp_out = 0;
  if (start + 1 >= text.size())
    return false;

  const unsigned char b0 = (unsigned char)text[start];
  const unsigned char b1 = (unsigned char)text[start + 1];
  if (b0 == 0xC2 && b1 >= 0x80 && b1 <= 0xBF) {
    if (consumed)
      *consumed = 2;
    if (cp_out)
      *cp_out = b1;
    return true;
  }
  if (b0 == 0xC3 && b1 >= 0x80 && b1 <= 0xBF) {
    if (consumed)
      *consumed = 2;
    if (cp_out)
      *cp_out = 0x40u + b1;
    return true;
  }
  return false;
}

static bool IsAllCapsHeading(const std::string &text) {
  int letters = 0;
  int upper = 0;
  for (size_t i = 0; i < text.size(); i++) {
    unsigned char c = (unsigned char)text[i];
    if (c >= 'A' && c <= 'Z') {
      letters++;
      upper++;
    } else if (c >= 'a' && c <= 'z') {
      letters++;
    }
  }
  return letters >= 4 && upper == letters;
}

static bool StartsLikeListOrHeading(const std::string &text) {
  if (text.empty())
    return true;
  unsigned char c0 = (unsigned char)text[0];
  if (c0 == '-' || c0 == '*' || c0 == 0xE2)
    return true;
  if (std::isdigit(c0) != 0)
    return true;
  return IsAllCapsHeading(text);
}

static bool EndsWithSentenceBreak(const std::string &text) {
  for (size_t i = text.size(); i > 0; i--) {
    unsigned char c = (unsigned char)text[i - 1];
    if (std::isspace(c) != 0)
      continue;
    return c == '.' || c == '!' || c == '?';
  }
  return false;
}

static bool StartsWithLowercase(const std::string &text) {
  for (size_t i = 0; i < text.size(); i++) {
    unsigned char c = (unsigned char)text[i];
    if (std::isspace(c) != 0 || c == '"' || c == '\'' || c == '(' ||
        c == '[') {
      continue;
    }
    if (c >= 'a' && c <= 'z')
      return true;
    // Common Spanish lowercase initials in UTF-8: áéíóúñü.
    if (i + 1 < text.size() && c == 0xC3) {
      unsigned char c1 = (unsigned char)text[i + 1];
      switch (c1) {
      case 0xA1:
      case 0xA9:
      case 0xAD:
      case 0xB3:
      case 0xBA:
      case 0xB1:
      case 0xBC:
        return true;
      default:
        break;
      }
    }
    return false;
  }
  return false;
}

static size_t CountWords(const std::string &text) {
  size_t words = 0;
  bool in_word = false;
  for (size_t i = 0; i < text.size(); i++) {
    unsigned char c = (unsigned char)text[i];
    bool word_char = std::isalnum(c) != 0 || c >= 0x80;
    if (word_char && !in_word) {
      words++;
      in_word = true;
    } else if (!word_char) {
      in_word = false;
    }
  }
  return words;
}

static bool ShouldMergeBlocks(const std::string &left, const std::string &right,
                              int separator_blank_lines) {
  // This is intentionally conservative: only join short prose fragments that
  // look like a wrapped sentence continuation, not headings or finished ideas.
  if (left.empty() || right.empty())
    return false;
  if (separator_blank_lines > 1)
    return false;
  if (left.size() < 20 || right.size() < 8)
    return false;
  if (CountWords(left) < 3 || CountWords(right) < 2)
    return false;
  if (StartsLikeListOrHeading(left) || StartsLikeListOrHeading(right))
    return false;
  if (EndsWithSentenceBreak(left))
    return false;
  return StartsWithLowercase(right);
}

static void AppendMergedSpace(std::string *out) {
  if (!out || out->empty())
    return;
  if (out->back() == '-') {
    out->pop_back();
    return;
  }
  // Only strip a real soft hyphen (U+00AD encoded as C2 AD), not any UTF-8
  // codepoint whose trailing byte happens to be 0xAD (for example "í").
  if (out->size() >= 2 && (unsigned char)(*out)[out->size() - 2] == 0xC2 &&
      (unsigned char)(*out)[out->size() - 1] == 0xAD) {
    out->resize(out->size() - 2);
    return;
  }
  if (out->back() != ' ')
    out->push_back(' ');
}

} // namespace

void RepairCommonMojibakeInPlace(std::string *text) {
  if (!text)
    return;
  size_t write = 0;
  for (size_t read = 0; read < text->size();) {
    if (read + 1 < text->size() && (unsigned char)(*text)[read] == 0xC3 &&
        (unsigned char)(*text)[read + 1] == 0x83) {
      size_t consumed = 0;
      unsigned int cp = 0;
      if (DecodeLatin1Supplement(*text, read + 2, &consumed, &cp) &&
          cp >= 0xA0 && cp <= 0xBF) {
        // The repaired U+00E0..U+00FF is always a two-byte UTF-8 sequence.
        (*text)[write++] = (char)0xC3;
        (*text)[write++] = (char)cp;
        read += 2 + consumed;
        continue;
      }
    }
    (*text)[write++] = (*text)[read++];
  }
  text->resize(write);
}

std::string RepairCommonMojibake(const std::string &text) {
  std::string out = text;
  RepairCommonMojibakeInPlace(&out);
  return out;
}

std::string RepairCommonMojibakePreservingMobiImageTokens(
    const std::string &text) {
  // Single-pass implementation: scan once, copy image tokens verbatim and
  // repair mojibake in text regions. Avoids the tokenize-then-repair pattern
  // which creates a vector of string copies before doing any repair work.
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const size_t token_len = MobiImageTokenLengthAt(text, i);
    if (token_len > 0) {
      out.append(text, i, token_len);
      i += token_len;
      continue;
    }
    // Same mojibake repair logic as RepairCommonMojibake.
    if (i + 1 < text.size() && (unsigned char)text[i] == 0xC3 &&
        (unsigned char)text[i + 1] == 0x83) {
      size_t consumed = 0;
      unsigned int cp = 0;
      if (DecodeLatin1Supplement(text, i + 2, &consumed, &cp)) {
        if (cp >= 0xA0 && cp <= 0xBF) {
          AppendUtf8Codepoint(&out, 0x00C0u + (cp - 0x80u));
          i += 2 + consumed;
          continue;
        }
      }
    }
    out.push_back(text[i]);
    i++;
  }
  return out;
}

std::string FixBrokenParagraphWraps(const std::string &text) {
  // Keep only the current fragment and the preceding merged paragraph;
  // collecting every trimmed line duplicates the complete book in memory.
  std::string out;
  std::string previous;
  std::string line;
  bool have_block = false;
  int pending_blank_lines = 0;

  size_t line_start = 0;
  while (line_start <= text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos)
      line_end = text.size();

    size_t begin = line_start;
    size_t end = line_end;
    while (begin < end && std::isspace((unsigned char)text[begin]) != 0)
      ++begin;
    while (end > begin && std::isspace((unsigned char)text[end - 1]) != 0)
      --end;
    line.assign(text, begin, end - begin);
    if (line.empty()) {
      ++pending_blank_lines;
    } else {
      if (!have_block) {
        out.reserve(text.size());
        out = line;
        previous = line;
        have_block = true;
      } else if (ShouldMergeBlocks(previous, line, pending_blank_lines)) {
        AppendMergedSpace(&out);
        out += line;
        previous += " ";
        previous += line;
      } else {
        if (pending_blank_lines > 0)
          out.append("\n\n");
        else
          out.push_back('\n');
        out += line;
        previous = line;
      }
      pending_blank_lines = 0;
    }

    if (line_end == text.size())
      break;
    line_start = line_end + 1;
  }

  if (!have_block)
    return text;
  return out;
}

std::string FixBrokenParagraphWrapsPreservingMobiImageTokens(
    const std::string &text) {
  return ApplyCleanupPreservingMobiImageTokens(
      text, [](const std::string &piece) {
        return FixBrokenParagraphWraps(piece);
      });
}

namespace {

// Token at `pos` per the parser's rule (the id is read from image_pos+1).
size_t ScanTokenAt(const std::string &t, size_t pos, size_t *image_pos) {
  const unsigned char c0 = (unsigned char)t[pos];
  if (c0 == kTextImage)
    return pos + 2 < t.size() ? (*image_pos = pos, 3u) : 0u;
  if (IsMobiImageContextByte(c0) && pos + 3 < t.size() &&
      (unsigned char)t[pos + 1] == kTextImage)
    return (*image_pos = pos + 1, 4u);
  return 0;
}

// Summary of a merged block: everything ShouldMergeBlocks asks of `left`.
struct BlockSummary {
  size_t size, words, letters, upper;
  unsigned char first, last;
};

static void CountLetters(const std::string &s, size_t *letters, size_t *upper) {
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 'A' && c <= 'Z') {
      ++*letters;
      ++*upper;
    } else if (c >= 'a' && c <= 'z')
      ++*letters;
  }
}

static BlockSummary Summarize(const std::string &line) {
  BlockSummary b = {line.size(), CountWords(line), 0, 0,
                    (unsigned char)line[0], (unsigned char)line[line.size() - 1]};
  CountLetters(line, &b.letters, &b.upper);
  return b;
}

static void AppendToSummary(BlockSummary *b, const std::string &line) {
  b->size += 1 + line.size();
  b->words += CountWords(line);
  CountLetters(line, &b->letters, &b->upper);
  b->last = (unsigned char)line[line.size() - 1];
}

static bool SummaryStartsLikeListOrHeading(const BlockSummary &b) {
  if (b.first == '-' || b.first == '*' || b.first == 0xE2 ||
      std::isdigit(b.first) != 0)
    return true;
  return b.letters >= 4 && b.upper == b.letters;
}

static bool ShouldMergeSummary(const BlockSummary &left,
                               const std::string &right,
                               int separator_blank_lines) {
  if (right.empty())
    return false;
  if (separator_blank_lines > 1)
    return false;
  if (left.size < 20 || right.size() < 8)
    return false;
  if (left.words < 3 || CountWords(right) < 2)
    return false;
  if (SummaryStartsLikeListOrHeading(left) || StartsLikeListOrHeading(right))
    return false;
  if (left.last == '.' || left.last == '!' || left.last == '?')
    return false;
  return StartsWithLowercase(right);
}

static std::string TrimmedLine(const std::string &raw) {
  size_t begin = 0, end = raw.size();
  while (begin < end && std::isspace((unsigned char)raw[begin]) != 0)
    ++begin;
  while (end > begin && std::isspace((unsigned char)raw[end - 1]) != 0)
    --end;
  return raw.substr(begin, end - begin);
}

// Streaming FixBrokenParagraphWraps. A "piece" is the whole text, or with
// token_aware each text run between inline image tokens (which the preserving
// variant cleans on its own, keeping the run's edge whitespace).
class WrapStage {
public:
  explicit WrapStage(bool token_aware) : token_aware_(token_aware) {
    ResetPiece();
  }
  void Feed(const char *data, size_t n, bool final, std::string *out) {
    std::string work;
    work.swap(carry_);
    work.append(data, n);
    size_t i = 0;
    while (i < work.size()) {
      if (!final && work.size() - i < 4)
        break;
      size_t token_len = 0;
      if (token_aware_)
        token_len = MobiImageTokenLengthAt(work, i);
      if (token_len) {
        EndPiece(out);
        out->append(work, i, token_len);
        i += token_len;
        continue;
      }
      Char((unsigned char)work[i++], out);
    }
    if (!final && i < work.size())
      carry_.assign(work, i, std::string::npos);
    if (final)
      EndPiece(out);
  }

private:
  void ResetPiece() {
    leading_ = true;
    have_block_ = false;
    blank_lines_ = 0;
    lead_.clear();
    line_.clear();
    trail_.clear();
    held_.clear();
  }
  void Emit(std::string *out, bool all) {
    const size_t cut = all ? held_.size() : held_.size() > 2 ? held_.size() - 2 : 0;
    if (cut) {
      out->append(held_, 0, cut);
      held_.erase(0, cut);
    }
  }
  void EndLine(std::string *out) {
    const std::string t = TrimmedLine(line_);
    line_.clear();
    if (t.empty()) {
      ++blank_lines_;
      return;
    }
    if (!have_block_) {
      held_ = t;
      summary_ = Summarize(t);
      have_block_ = true;
    } else if (ShouldMergeSummary(summary_, t, blank_lines_)) {
      AppendMergedSpace(&held_);
      held_ += t;
      AppendToSummary(&summary_, t);
    } else {
      held_ += blank_lines_ > 0 ? "\n\n" : "\n";
      held_ += t;
      summary_ = Summarize(t);
    }
    blank_lines_ = 0;
    Emit(out, false);
  }
  void Char(unsigned char c, std::string *out) {
    const bool space = std::isspace(c) != 0;
    if (leading_) {
      if (space) {
        lead_.push_back((char)c);
        return;
      }
      if (token_aware_)
        out->append(lead_);
      lead_.clear();
      leading_ = false;
    }
    if (space)
      trail_.push_back((char)c);
    else
      trail_.clear();
    if (c == '\n')
      EndLine(out);
    else
      line_.push_back((char)c);
  }
  void EndPiece(std::string *out) {
    if (leading_) {
      // Empty or whitespace-only: returned unchanged by both variants.
      out->append(lead_);
    } else {
      EndLine(out);
      Emit(out, true);
      if (token_aware_)
        out->append(trail_);
    }
    ResetPiece();
  }
  bool token_aware_, leading_, have_block_;
  int blank_lines_;
  BlockSummary summary_;
  std::string carry_, lead_, line_, trail_, held_;
};

class MojibakeStage {
public:
  explicit MojibakeStage(bool token_aware) : token_aware_(token_aware) {}
  void Feed(const char *data, size_t n, bool final, std::string *out) {
    std::string work;
    work.swap(carry_);
    work.append(data, n);
    size_t i = 0;
    while (i < work.size()) {
      if (!final && work.size() - i < 4)
        break;
      if (token_aware_) {
        const size_t token_len = MobiImageTokenLengthAt(work, i);
        if (token_len) {
          out->append(work, i, token_len);
          i += token_len;
          continue;
        }
      }
      if (i + 1 < work.size() && (unsigned char)work[i] == 0xC3 &&
          (unsigned char)work[i + 1] == 0x83) {
        size_t consumed = 0;
        unsigned int cp = 0;
        if (DecodeLatin1Supplement(work, i + 2, &consumed, &cp) && cp >= 0xA0 &&
            cp <= 0xBF) {
          out->push_back((char)0xC3);
          out->push_back((char)cp);
          i += 2 + consumed;
          continue;
        }
      }
      out->push_back(work[i++]);
    }
    if (!final && i < work.size())
      carry_.assign(work, i, std::string::npos);
  }

private:
  bool token_aware_;
  std::string carry_;
};

} // namespace

TokenScan::TokenScan() {}
void TokenScan::Feed(const char *data, size_t n, bool final) {
  std::string work;
  work.swap(carry_);
  work.append(data, n);
  size_t i = 0;
  while (i < work.size()) {
    if (!final && work.size() - i < 4)
      break;
    size_t image_pos = 0;
    const size_t len = ScanTokenAt(work, i, &image_pos);
    if (!len) {
      ++i;
      continue;
    }
    ids_.push_back((unsigned short)(((unsigned char)work[image_pos + 1] << 8) |
                                    (unsigned char)work[image_pos + 2]));
    i += len;
  }
  if (!final && i < work.size())
    carry_.assign(work, i, std::string::npos);
}

struct CleanupStream::State {
  StreamOptions options;
  bool pending_cr = false;
  MojibakeStage mojibake;
  WrapStage wrap;
  TokenScan before, after;
  std::string a, b, c;
  explicit State(const StreamOptions &o)
      : options(o), mojibake(o.token_aware), wrap(o.token_aware) {}
};
CleanupStream::CleanupStream(const StreamOptions &options)
    : state_(new State(options)) {}
CleanupStream::~CleanupStream() { delete state_; }
const std::vector<unsigned short> &CleanupStream::IdsBefore() const {
  return state_->before.Ids();
}
const std::vector<unsigned short> &CleanupStream::IdsAfter() const {
  return state_->after.Ids();
}
void CleanupStream::Feed(const char *data, size_t n, bool final,
                         std::string *out) {
  State &s = *state_;
  // NormalizeNewlines: "\r\n" and a lone "\r" become "\n".
  std::string &a = s.a;
  a.clear();
  for (size_t i = 0; i < n; ++i) {
    const char c = data[i];
    if (s.pending_cr) {
      s.pending_cr = false;
      a.push_back('\n');
      if (c == '\n')
        continue;
    }
    if (c == '\r')
      s.pending_cr = true;
    else
      a.push_back(c);
  }
  if (final && s.pending_cr) {
    s.pending_cr = false;
    a.push_back('\n');
  }
  const std::string *cur = &a;
  if (s.options.token_aware)
    s.before.Feed(a.data(), a.size(), final);
  if (s.options.mojibake) {
    s.b.clear();
    s.mojibake.Feed(cur->data(), cur->size(), final, &s.b);
    cur = &s.b;
  }
  if (s.options.wrap_fix) {
    s.c.clear();
    s.wrap.Feed(cur->data(), cur->size(), final, &s.c);
    cur = &s.c;
  }
  if (s.options.token_aware)
    s.after.Feed(cur->data(), cur->size(), final);
  out->append(*cur);
}

} // namespace mobi_text_cleanup

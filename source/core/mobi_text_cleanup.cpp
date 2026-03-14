#include "mobi_text_cleanup.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace mobi_text_cleanup {

namespace {

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

static std::string TrimAscii(const std::string &text) {
  size_t begin = 0;
  while (begin < text.size() &&
         std::isspace((unsigned char)text[begin]) != 0) {
    begin++;
  }

  size_t end = text.size();
  while (end > begin && std::isspace((unsigned char)text[end - 1]) != 0) {
    end--;
  }
  return text.substr(begin, end - begin);
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

std::string RepairCommonMojibake(const std::string &text) {
  std::string out;
  out.reserve(text.size());

  for (size_t i = 0; i < text.size();) {
    // UTF-8 mojibake of Latin-1 supplements often appears as "Ã" followed by a
    // re-encoded Latin-1 byte. Rebuild the original two-byte UTF-8 sequence.
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
  // Work on already-decoded plain text so the repair can clean either <br>
  // wraps or block-tag-per-line conversions with the same heuristic.
  std::vector<std::string> blocks;
  std::vector<int> separators;
  std::string current;
  int pending_blank_lines = 0;

  size_t line_start = 0;
  while (line_start <= text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos)
      line_end = text.size();

    std::string line = TrimAscii(text.substr(line_start, line_end - line_start));
    if (line.empty()) {
      pending_blank_lines++;
    } else {
      if (!blocks.empty())
        separators.push_back(pending_blank_lines);
      blocks.push_back(line);
      pending_blank_lines = 0;
    }

    if (line_end == text.size())
      break;
    line_start = line_end + 1;
  }

  if (blocks.empty())
    return text;

  std::string out = blocks[0];
  std::string previous = blocks[0];
  for (size_t i = 1; i < blocks.size(); i++) {
    const int blank_lines = separators[i - 1];
    if (ShouldMergeBlocks(previous, blocks[i], blank_lines)) {
      AppendMergedSpace(&out);
      out += blocks[i];
      previous += " ";
      previous += blocks[i];
      continue;
    }

    if (blank_lines > 0)
      out.append("\n\n");
    else
      out.push_back('\n');
    out += blocks[i];
    previous = blocks[i];
  }

  return out;
}

} // namespace mobi_text_cleanup

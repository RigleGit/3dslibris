#include "formats/markdown/markdown_parser.h"

#include "book/book.h"
#include "formats/common/book_error.h"
#include "formats/common/file_read_utils.h"
#include "formats/common/plain_parser.h"
#include "formats/common/text_helpers.h"
#include "formats/mobi/mobi_heading_markers.h"
#include "shared/debug_log.h"
#include "shared/text_token_constants.h"

#include <ctype.h>
#include <string.h>
#include <string>
#include <utility>

namespace markdown_parser {
namespace {

static const size_t kMarkdownMaxBytes = 8u * 1024u * 1024u;

bool IsSpace(char c) { return c == ' ' || c == '\t'; }

std::string TrimLeft(const std::string &s) {
  size_t pos = 0;
  while (pos < s.size() && IsSpace(s[pos]))
    pos++;
  return s.substr(pos);
}

std::string TrimRight(const std::string &s) {
  size_t end = s.size();
  while (end > 0 && IsSpace(s[end - 1]))
    end--;
  return s.substr(0, end);
}

bool IsFenceLine(const std::string &trimmed) {
  return trimmed.size() >= 3 &&
         (trimmed.compare(0, 3, "```") == 0 ||
          trimmed.compare(0, 3, "~~~") == 0);
}

int ParseAtxHeading(const std::string &trimmed, size_t *content_pos) {
  size_t hashes = 0;
  while (hashes < trimmed.size() && trimmed[hashes] == '#')
    hashes++;
  if (hashes == 0 || hashes > 6)
    return 0;
  if (hashes >= trimmed.size() || !IsSpace(trimmed[hashes]))
    return 0;
  if (content_pos)
    *content_pos = hashes + 1;
  return hashes > 3 ? 3 : (int)hashes;
}

bool IsSetextUnderline(const std::string &trimmed, int *level) {
  if (trimmed.empty())
    return false;
  char marker = trimmed[0];
  if (marker != '=' && marker != '-')
    return false;
  for (size_t i = 0; i < trimmed.size(); i++) {
    if (trimmed[i] != marker)
      return false;
  }
  if (level)
    *level = (marker == '=') ? 1 : 2;
  return true;
}

bool ParseUnorderedList(const std::string &trimmed, size_t *content_pos) {
  if (trimmed.size() < 2)
    return false;
  char marker = trimmed[0];
  if (marker != '-' && marker != '*' && marker != '+')
    return false;
  if (!IsSpace(trimmed[1]))
    return false;
  if (content_pos)
    *content_pos = 2;
  return true;
}

bool ParseOrderedList(const std::string &trimmed, size_t *content_pos) {
  size_t pos = 0;
  while (pos < trimmed.size() && isdigit((unsigned char)trimmed[pos]))
    pos++;
  if (pos == 0 || pos >= trimmed.size())
    return false;
  if (trimmed[pos] != '.' && trimmed[pos] != ')')
    return false;
  pos++;
  if (pos >= trimmed.size() || !IsSpace(trimmed[pos]))
    return false;
  if (content_pos)
    *content_pos = pos + 1;
  return true;
}

std::string StripOptionalClosingHashes(const std::string &s) {
  std::string out = TrimRight(s);
  size_t end = out.size();
  size_t pos = end;
  while (pos > 0 && out[pos - 1] == '#')
    pos--;
  if (pos < end && pos > 0 && IsSpace(out[pos - 1]))
    out = TrimRight(out.substr(0, pos - 1));
  return out;
}

void AppendStyleToken(std::string *out, unsigned char token) {
  if (out)
    out->push_back((char)token);
}

bool CanOpenDelimiter(const std::string &s, size_t pos, size_t len) {
  if (pos + len >= s.size())
    return false;
  return !isspace((unsigned char)s[pos + len]);
}

bool CanCloseDelimiter(const std::string &s, size_t pos) {
  if (pos == 0)
    return false;
  return !isspace((unsigned char)s[pos - 1]);
}

std::string ConvertInlineMarkdown(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool bold = false;
  bool italic = false;
  bool mono = false;

  for (size_t i = 0; i < in.size(); i++) {
    char c = in[i];
    if (c == '\\' && i + 1 < in.size()) {
      out.push_back(in[++i]);
      continue;
    }
    if (c == '`') {
      AppendStyleToken(&out, mono ? TEXT_MONO_OFF : TEXT_MONO_ON);
      mono = !mono;
      continue;
    }
    if (!mono && i + 2 < in.size() && in.compare(i, 3, "***") == 0 &&
        ((bold && italic && CanCloseDelimiter(in, i)) ||
         (!bold && !italic && CanOpenDelimiter(in, i, 3)))) {
      AppendStyleToken(&out, italic ? TEXT_ITALIC_OFF : TEXT_ITALIC_ON);
      AppendStyleToken(&out, bold ? TEXT_BOLD_OFF : TEXT_BOLD_ON);
      italic = !italic;
      bold = !bold;
      i += 2;
      continue;
    }
    if (!mono && i + 1 < in.size() &&
        ((in[i] == '*' && in[i + 1] == '*') ||
         (in[i] == '_' && in[i + 1] == '_')) &&
        ((bold && CanCloseDelimiter(in, i)) ||
         (!bold && CanOpenDelimiter(in, i, 2)))) {
      AppendStyleToken(&out, bold ? TEXT_BOLD_OFF : TEXT_BOLD_ON);
      bold = !bold;
      i++;
      continue;
    }
    if (!mono && (c == '*' || c == '_') &&
        ((italic && CanCloseDelimiter(in, i)) ||
         (!italic && CanOpenDelimiter(in, i, 1)))) {
      AppendStyleToken(&out, italic ? TEXT_ITALIC_OFF : TEXT_ITALIC_ON);
      italic = !italic;
      continue;
    }
    if (!mono && c == '[') {
      size_t close = in.find(']', i + 1);
      if (close != std::string::npos && close + 1 < in.size() &&
          in[close + 1] == '(') {
        size_t end = in.find(')', close + 2);
        if (end != std::string::npos) {
          out.append(ConvertInlineMarkdown(in.substr(i + 1, close - i - 1)));
          i = end;
          continue;
        }
      }
    }
    if (!mono && c == '!' && i + 1 < in.size() && in[i + 1] == '[') {
      size_t close = in.find(']', i + 2);
      if (close != std::string::npos && close + 1 < in.size() &&
          in[close + 1] == '(') {
        size_t end = in.find(')', close + 2);
        if (end != std::string::npos) {
          out.append(ConvertInlineMarkdown(in.substr(i + 2, close - i - 2)));
          i = end;
          continue;
        }
      }
    }
    out.push_back(c);
  }

  if (mono)
    AppendStyleToken(&out, TEXT_MONO_OFF);
  if (italic)
    AppendStyleToken(&out, TEXT_ITALIC_OFF);
  if (bold)
    AppendStyleToken(&out, TEXT_BOLD_OFF);
  return out;
}

void AppendMarkdownLine(const std::string &line, std::string *out) {
  out->append(ConvertInlineMarkdown(line));
  out->push_back('\n');
}

void AppendHeading(int level, const std::string &title, std::string *out) {
  out->push_back((char)mobi_heading_markers::MarkerForHeadingLevel(level));
  AppendStyleToken(out, TEXT_BOLD_ON);
  out->append(ConvertInlineMarkdown(title));
  AppendStyleToken(out, TEXT_BOLD_OFF);
  out->push_back('\n');
  out->push_back('\n');
}

bool ConvertMarkdownToPlainTokens(const std::string &markdown,
                                  std::string *out) {
  if (!out)
    return false;
  out->clear();
  out->reserve(markdown.size());

  bool in_fence = false;
  std::string pending_setext;
  size_t cursor = 0;
  while (cursor <= markdown.size()) {
    size_t end = markdown.find('\n', cursor);
    std::string line =
        end == std::string::npos ? markdown.substr(cursor)
                                 : markdown.substr(cursor, end - cursor);
    if (!line.empty() && line[line.size() - 1] == '\r')
      line.resize(line.size() - 1);
    cursor = (end == std::string::npos) ? markdown.size() + 1 : end + 1;

    std::string trimmed_left = TrimLeft(line);
    std::string trimmed = TrimRight(trimmed_left);

    if (IsFenceLine(trimmed)) {
      if (!pending_setext.empty()) {
        AppendMarkdownLine(pending_setext, out);
        pending_setext.clear();
      }
      in_fence = !in_fence;
      continue;
    }
    if (in_fence) {
      out->push_back((char)TEXT_MONO_ON);
      out->append(line);
      out->push_back((char)TEXT_MONO_OFF);
      out->push_back('\n');
      continue;
    }
    if (!pending_setext.empty()) {
      int setext_level = 0;
      if (IsSetextUnderline(trimmed, &setext_level)) {
        AppendHeading(setext_level, pending_setext, out);
        pending_setext.clear();
        continue;
      }
      AppendMarkdownLine(pending_setext, out);
      pending_setext.clear();
    }
    if (trimmed.empty()) {
      out->push_back('\n');
      continue;
    }

    size_t content_pos = 0;
    int heading_level = ParseAtxHeading(trimmed, &content_pos);
    if (heading_level > 0) {
      AppendHeading(heading_level,
                    StripOptionalClosingHashes(trimmed.substr(content_pos)),
                    out);
      continue;
    }
    if (trimmed[0] == '>' &&
        (trimmed.size() == 1 || IsSpace(trimmed[1]))) {
      std::string quote = trimmed.size() > 1 ? TrimLeft(trimmed.substr(1)) : "";
      out->append("> ");
      AppendMarkdownLine(quote, out);
      continue;
    }
    if (ParseUnorderedList(trimmed, &content_pos)) {
      out->append("- ");
      AppendMarkdownLine(trimmed.substr(content_pos), out);
      continue;
    }
    if (ParseOrderedList(trimmed, &content_pos)) {
      out->append("- ");
      AppendMarkdownLine(trimmed.substr(content_pos), out);
      continue;
    }

    pending_setext = trimmed;
  }
  if (!pending_setext.empty())
    AppendMarkdownLine(pending_setext, out);
  return true;
}

} // namespace

uint8_t Parse(Book *book, const char *path) {
  if (!book || !path)
    return BOOK_ERR_CORRUPT;

  std::string raw;
  if (!file_read_utils::ReadPathToStringLimited(path, &raw,
                                                kMarkdownMaxBytes)) {
#ifdef DSLIBRIS_DEBUG
    int err = file_read_utils::LastErrorNumber();
    DBG_LOGF(book ? book->GetStatusReporter() : NULL,
             "Markdown read failed path=%s op=%s errno=%d strerror=%s", path,
             file_read_utils::LastErrorOperation(), err,
             err ? strerror(err) : "none");
#endif
    return BOOK_ERR_CORRUPT;
  }
  if (raw.empty())
    return BOOK_ERR_CORRUPT;

  NormalizeNewlines(&raw);
  std::string normalized = NormalizeTextUtf8(std::move(raw));
  std::string plain_tokens;
  if (!ConvertMarkdownToPlainTokens(normalized, &plain_tokens))
    return BOOK_ERR_CORRUPT;

  return plain_parser::ParseBuffer(book, plain_tokens, false);
}

} // namespace markdown_parser

#include "formats/mobi/mobi_safe_markup_extract.h"

#include <cctype>
#include <stdint.h>
#include <string>

#include "formats/common/html_entity_utils.h"
#include "shared/text_token_constants.h"

bool mobi_extract_image_recindex(const std::string &tag, uint16_t *recindex_out);
std::string mobi_inline_image_path(uint16_t recindex);

namespace mobi_safe_markup_extract {
namespace {

static bool IsAsciiSpace(char c) {
  unsigned char uc = (unsigned char)c;
  return uc == ' ' || uc == '\t' || uc == '\n' || uc == '\r';
}

static void TrimTrailingInlineWhitespace(std::string *out) {
  if (!out)
    return;
  while (!out->empty() && (out->back() == ' ' || out->back() == '\t' ||
                           out->back() == '\r'))
    out->pop_back();
}

static void AppendParagraphBreak(std::string *out) {
  if (!out)
    return;
  TrimTrailingInlineWhitespace(out);
  while (!out->empty() && out->back() == '\n')
    out->pop_back();
  if (!out->empty())
    out->append("\n\n");
}

static void AppendSingleSpace(std::string *out) {
  if (!out || out->empty())
    return;
  char tail = out->back();
  if (tail != ' ' && tail != '\n' && tail != '\t' && tail != '\r')
    out->push_back(' ');
}

static std::string LowerAscii(std::string s) {
  for (size_t i = 0; i < s.size(); i++)
    s[i] = (char)std::tolower((unsigned char)s[i]);
  return s;
}

static std::string ParseTagName(const std::string &tag_text) {
  size_t i = 0;
  while (i < tag_text.size() && IsAsciiSpace(tag_text[i]))
    i++;
  if (i < tag_text.size() && tag_text[i] == '/')
    i++;
  while (i < tag_text.size() && IsAsciiSpace(tag_text[i]))
    i++;

  size_t start = i;
  while (i < tag_text.size()) {
    unsigned char c = (unsigned char)tag_text[i];
    if (IsAsciiSpace((char)c) || c == '/' || c == '>')
      break;
    i++;
  }
  return LowerAscii(tag_text.substr(start, i - start));
}

static bool IsClosingTag(const std::string &tag_text) {
  size_t i = 0;
  while (i < tag_text.size() && IsAsciiSpace(tag_text[i]))
    i++;
  return i < tag_text.size() && tag_text[i] == '/';
}

static bool IsBlockTag(const std::string &name) {
  return name == "p" || name == "div" || name == "section" ||
         name == "article" || name == "blockquote" || name == "h1" ||
         name == "h2" || name == "h3" || name == "h4" || name == "h5" ||
         name == "h6" || name == "tr";
}

static void AppendInlineImageToken(std::string *out, uint16_t image_id,
                                   bool at_paragraph_start) {
  if (!out)
    return;
  out->push_back((char)(at_paragraph_start ? TEXT_IMAGE_LEADING_PARAGRAPH
                                           : TEXT_IMAGE_CONTEXT_DEFAULT));
  out->push_back((char)TEXT_IMAGE);
  out->push_back((char)((image_id >> 8) & 0xFF));
  out->push_back((char)(image_id & 0xFF));
}

} // namespace

std::string ExtractToText(const std::string &markup_utf8,
                          const InlineImageCallbacks &image_callbacks) {
  std::string out;
  out.reserve(markup_utf8.size());

  bool in_script = false;
  bool in_style = false;
  bool pending_space = false;
  bool at_paragraph_start = true;

  for (size_t i = 0; i < markup_utf8.size();) {
    const unsigned char c = (unsigned char)markup_utf8[i];

    if (c == '<') {
      size_t close = markup_utf8.find('>', i + 1);
      if (close == std::string::npos)
        break;
      const std::string tag_text = markup_utf8.substr(i + 1, close - i - 1);
      const std::string tag_name = ParseTagName(tag_text);
      const bool closing = IsClosingTag(tag_text);

      if (tag_name == "script") {
        in_script = !closing;
      } else if (tag_name == "style") {
        in_style = !closing;
      } else if (!in_script && !in_style) {
        if (tag_name == "br") {
          TrimTrailingInlineWhitespace(&out);
          if (!out.empty() && out.back() != '\n')
            out.push_back('\n');
          pending_space = false;
        } else if (tag_name == "img" && !closing) {
          const std::string tag = markup_utf8.substr(i + 1, close - i - 1);
          uint16_t recindex = 0;
          if (image_callbacks.register_inline_image &&
              mobi_extract_image_recindex(tag, &recindex)) {
            const uint16_t image_id = image_callbacks.register_inline_image(
                image_callbacks.user_data, mobi_inline_image_path(recindex));
            if (pending_space) {
              AppendSingleSpace(&out);
              pending_space = false;
            }
            AppendInlineImageToken(&out, image_id, at_paragraph_start);
            at_paragraph_start = false;
          }
        } else if (tag_name == "li" && !closing) {
          AppendParagraphBreak(&out);
          out.append("- ");
          pending_space = false;
          at_paragraph_start = false;
        } else if (IsBlockTag(tag_name)) {
          AppendParagraphBreak(&out);
          pending_space = false;
          at_paragraph_start = true;
        }
      }

      i = close + 1;
      continue;
    }

    if (in_script || in_style) {
      i++;
      continue;
    }

    if (c == '&') {
      size_t semi = markup_utf8.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 12) {
        std::string decoded;
        if (html_entity_utils::DecodeHtmlEntityUtf8(
                markup_utf8.substr(i + 1, semi - i - 1), &decoded)) {
          if (pending_space) {
            AppendSingleSpace(&out);
            pending_space = false;
          }
          out += decoded;
          if (!decoded.empty() &&
              decoded.find_first_not_of(" \t\r\n") != std::string::npos) {
            at_paragraph_start = false;
          }
          i = semi + 1;
          continue;
        }
      }
    }

    if (IsAsciiSpace((char)c)) {
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
      pending_space = false;
    }

    out.push_back((char)c);
    at_paragraph_start = false;
    i++;
  }

  TrimTrailingInlineWhitespace(&out);
  while (!out.empty() && out.back() == '\n')
    out.pop_back();
  return out;
}

} // namespace mobi_safe_markup_extract

/*
    3dslibris - book_xml_fb2_handler.cpp

    FB2-specific element handling.
    Extracted from book_xml_parser.cpp.
*/

#include "book/book_xml_fb2_handler.h"

#include "book/book.h"
#include "book/book_xml_parser_support.h"
#include "parse.h"

#include <string.h>
#include <string>

namespace {

static const size_t kFb2BinaryMaxChars = 6 * 1024 * 1024;

static bool XmlNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle))
    return true;
  // Case-insensitive fallback
  const char *a = name;
  const char *b = needle;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a;
    unsigned char cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb) break;
    a++; b++;
  }
  if (*a == '\0' && *b == '\0')
    return true;
  // Strip namespace prefix and retry
  const char *colon = strrchr(name, ':');
  return (colon && strcmp(colon + 1, needle) == 0);
}

static std::string NormalizeFb2ChapterTitle(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool pending_space = false;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
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

} // namespace

namespace book_xml_fb2_handler {

void HandleFb2SectionStart(parsedata_t *p, const char *el) {
  if (!p || !el || !p->fb2_mode || !parse_in(p, TAG_BODY))
    return;

  if (XmlNameEquals(el, "section")) {
    if (p->fb2_section_depth < 31)
      p->fb2_section_depth++;
    if (p->fb2_section_depth >= 0 && p->fb2_section_depth < 32)
      p->fb2_section_has_chapter[p->fb2_section_depth] = false;
  } else if (XmlNameEquals(el, "title") && p->fb2_section_depth > 0) {
    p->fb2_title_depth++;
    if (p->fb2_title_depth == 1 && p->fb2_title_capture_depth == 0 &&
        p->fb2_section_depth < 32 &&
        !p->fb2_section_has_chapter[p->fb2_section_depth]) {
      p->fb2_title_capture_depth = p->fb2_section_depth;
      p->fb2_title_text.clear();
    }
  }
}

bool HandleFb2BinaryStart(parsedata_t *p, const char *el, const char **attr) {
  if (!p || !el || !XmlNameEquals(el, "binary"))
    return false;

  parse_push(p, TAG_UNKNOWN);

  p->collecting_fb2_binary = false;
  p->fb2_binary_too_large = false;
  p->fb2_binary_id.clear();
  p->fb2_binary_data.clear();

  if (p->book && !p->book->GetEbookImagesEnabled()) {
    // Consume binary markup without collecting/decoding it or exposing Base64
    // as book text. Cover extraction uses its own independent parser.
    book_xml_parser_support::SetCurrentStackHidden(p, true);
    return true;
  }

  const char *id = NULL;
  for (int i = 0; attr && attr[i]; i += 2) {
    if (XmlNameEquals(attr[i], "id")) {
      id = attr[i + 1];
    }
  }

  if (id && *id && p->book) {
    p->collecting_fb2_binary = true;
    p->fb2_binary_id = id;
    if (!p->fb2_binary_id.empty() && p->fb2_binary_id[0] == '#')
      p->fb2_binary_id.erase(0, 1);
  }

  return true;
}

bool HandleFb2Chardata(parsedata_t *p, const char *txt, int txtlen) {
  if (!p || !txt || txtlen <= 0)
    return false;

  if (p->collecting_fb2_binary) {
    if (!p->fb2_binary_too_large) {
      for (int i = 0; i < txtlen; i++) {
        unsigned char c = (unsigned char)txt[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
          continue;
        p->fb2_binary_data.push_back((char)c);
      }
      if (p->fb2_binary_data.size() > kFb2BinaryMaxChars) {
        p->fb2_binary_data.clear();
        p->fb2_binary_too_large = true;
      }
    }
    return true;
  }

  if (parse_in(p, TAG_TITLE)) {
    if (p->fb2_mode && p->fb2_title_capture_depth > 0) {
      p->fb2_title_text.append(txt, (size_t)txtlen);
    } else {
      p->doc_title.append(txt, (size_t)txtlen);
    }
    return true;
  }

  return false;
}

bool HandleFb2BinaryEnd(parsedata_t *p, const char *el) {
  if (!p || !el || !XmlNameEquals(el, "binary"))
    return false;

  if (p->collecting_fb2_binary && !p->fb2_binary_too_large && p->book &&
      !p->fb2_binary_id.empty() && !p->fb2_binary_data.empty()) {
    p->book->StoreFb2InlineImage(p->fb2_binary_id, p->fb2_binary_data);
  }
  p->collecting_fb2_binary = false;
  p->fb2_binary_too_large = false;
  p->fb2_binary_id.clear();
  p->fb2_binary_data.clear();
  parse_pop(p);
  return true;
}

void HandleFb2TitleSectionEnd(parsedata_t *p, const char *el) {
  if (!p || !el || !p->fb2_mode)
    return;

  if (XmlNameEquals(el, "title")) {
    if (p->fb2_title_depth > 0) {
      bool finishing_capture =
          (p->fb2_title_depth == 1 && p->fb2_title_capture_depth > 0 &&
           p->fb2_title_capture_depth == p->fb2_section_depth);
      if (finishing_capture && p->book) {
        std::string chapter_title = NormalizeFb2ChapterTitle(p->fb2_title_text);
        if (!chapter_title.empty()) {
          int level = p->fb2_section_depth > 0 ? p->fb2_section_depth - 1 : 0;
          if (level > 255)
            level = 255;
          p->book->AddChapter(p->book->GetPageCount(), chapter_title, (u8)level);
          if (p->fb2_section_depth >= 0 && p->fb2_section_depth < 32)
            p->fb2_section_has_chapter[p->fb2_section_depth] = true;
        }
        p->fb2_title_text.clear();
        p->fb2_title_capture_depth = 0;
      }
      p->fb2_title_depth--;
      if (p->fb2_title_depth < 0)
        p->fb2_title_depth = 0;
    }
  } else if (XmlNameEquals(el, "section")) {
    if (p->fb2_section_depth > 0) {
      if (p->fb2_section_depth < 32)
        p->fb2_section_has_chapter[p->fb2_section_depth] = false;
      p->fb2_section_depth--;
    }
    if (p->fb2_section_depth < 0)
      p->fb2_section_depth = 0;
    if (p->fb2_title_capture_depth > p->fb2_section_depth) {
      p->fb2_title_capture_depth = 0;
      p->fb2_title_text.clear();
    }
  }
}

} // namespace book_xml_fb2_handler

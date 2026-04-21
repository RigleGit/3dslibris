/*
    3dslibris - bookmark_menu.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Builds paged bookmark entries from the current book.
    - Normalizes labels and maps each row to target page numbers.
*/

#include "menus/bookmark_menu.h"

#include <stdio.h>

#include "app/app.h"
#include "book/book.h"
#include "book/page.h"
#include "shared/text_token_constants.h"
#include "shared/text_unicode_utils.h"

namespace {

static std::string TrimLabel(const std::string &in) {
  size_t start = 0;
  while (start < in.size() && isspace((unsigned char)in[start]))
    start++;
  size_t end = in.size();
  while (end > start && isspace((unsigned char)in[end - 1]))
    end--;
  return in.substr(start, end - start);
}

static std::string SanitizePreviewText(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool prev_space = true;

  for (size_t i = 0; i < in.size();) {
    unsigned char ch = in[i];

    // Normalize smart quotes (E2 80 xx) to ASCII, strip ™ (E2 84 xx)
    if (ch == 0xE2 && i + 2 < in.size()) {
      unsigned char b2 = in[i + 1];
      unsigned char b3 = in[i + 2];

      if (b2 == 0x80) {
        // U+2018/U+2019/U+201B → ASCII '
        if (b3 == 0x98 || b3 == 0x99 || b3 == 0x9B) {
          out.push_back('\'');
          prev_space = false;
          i += 3;
          continue;
        }
        // U+201C/U+201D/U+201E/U+201F → ASCII "
        if (b3 == 0x9C || b3 == 0x9D || b3 == 0x9E || b3 == 0x9F) {
          out.push_back('"');
          prev_space = false;
          i += 3;
          continue;
        }
        // Dashes (U+2010..U+2015) and ellipsis (U+2026) → space
        if ((b3 >= 0x90 && b3 <= 0x95) || b3 == 0xA6) {
          if (!prev_space && !out.empty()) {
            out.push_back(' ');
            prev_space = true;
          }
          i += 3;
          continue;
        }
      }

      if (b2 == 0x84) {
        i += 3;
        continue;
      }
    }

    if (ch < 0x20 || ch == 0x7F) {
      if (!prev_space && !out.empty()) {
        out.push_back(' ');
        prev_space = true;
      }
      i++;
      continue;
    }

    if (isspace(ch)) {
      if (!prev_space && !out.empty()) {
        out.push_back(' ');
        prev_space = true;
      }
      i++;
      continue;
    }

    out.push_back(in[i]);
    prev_space = false;
    i++;
  }

  return TrimLabel(out);
}

static std::string ExtractPagePreview(Page *page, size_t max_chars) {
  if (!page || !page->GetBuffer() || page->GetLength() <= 0)
    return "";

  const u32 *buf = page->GetBuffer();
  const int len = page->GetLength();
  std::string out;
  out.reserve(max_chars + 16);
  bool prev_space = false;

  for (int i = 0; i < len && out.size() < max_chars; i++) {
    u32 c = buf[i];

    if (c == TEXT_IMAGE_CONTEXT_DEFAULT ||
        c == TEXT_IMAGE_LEADING_PARAGRAPH ||
        c == TEXT_IMAGE_FIGURE_WITH_CAPTION) {
      continue;
    }
    if (c == TEXT_IMAGE) {
      i += (i + 1 < len) ? 1 : 0;
      if (!prev_space && !out.empty()) {
        out.push_back(' ');
        prev_space = true;
      }
      continue;
    }
    if (c == TEXT_RTL_LINE_PX) {
      i += (i + 1 < len) ? 1 : 0;
      continue;
    }
    if (c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF || c == TEXT_ITALIC_ON ||
        c == TEXT_ITALIC_OFF || c == TEXT_UNDERLINE_ON ||
        c == TEXT_UNDERLINE_OFF || c == TEXT_OVERLINE_ON ||
        c == TEXT_OVERLINE_OFF || c == TEXT_STRIKETHROUGH_ON ||
        c == TEXT_STRIKETHROUGH_OFF || c == TEXT_SUPERSCRIPT_ON ||
        c == TEXT_SUPERSCRIPT_OFF || c == TEXT_SUBSCRIPT_ON ||
        c == TEXT_SUBSCRIPT_OFF) {
      continue;
    }
    if (c == TEXT_UNDERLINE_STYLE) {
      i += (i + 1 < len) ? 1 : 0;
      continue;
    }
    if (c == TEXT_PRE_ON || c == TEXT_PRE_OFF || c >= 0x110000) {
      continue;
    }
    if (c < 0x20) {
      if (!prev_space && !out.empty()) {
        out.push_back(' ');
        prev_space = true;
      }
      continue;
    }

    if (c < 0x80) {
      out.push_back((char)c);
      prev_space = (c == ' ');
    } else {
      char utf8_buf[4];
      int utf8_len = 0;
      if (c < 0x800) {
        utf8_buf[0] = (char)(0xC0 | (c >> 6));
        utf8_buf[1] = (char)(0x80 | (c & 0x3F));
        utf8_len = 2;
      } else if (c < 0x10000) {
        utf8_buf[0] = (char)(0xE0 | (c >> 12));
        utf8_buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        utf8_buf[2] = (char)(0x80 | (c & 0x3F));
        utf8_len = 3;
      } else {
        utf8_buf[0] = (char)(0xF0 | (c >> 18));
        utf8_buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        utf8_buf[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        utf8_buf[3] = (char)(0x80 | (c & 0x3F));
        utf8_len = 4;
      }
      for (int j = 0; j < utf8_len && out.size() < max_chars; j++)
        out.push_back(utf8_buf[j]);
      prev_space = false;
    }
  }

  return SanitizePreviewText(out);
}

static std::vector<std::string> WrapTextToLines(Text *ts,
                                                const std::string &text,
                                                int max_width,
                                                int max_lines) {
  std::vector<std::string> lines;
  std::string remaining = TrimLabel(text);
  const int style = TEXT_STYLE_BROWSER;

  while (!remaining.empty() && (int)lines.size() < max_lines) {
    u8 chars_fit =
        ts->GetCharCountInsideWidth(remaining.c_str(), style, (u8)max_width);
    if (chars_fit == 0)
      chars_fit = 1;

    size_t bytes =
        text_unicode_utils::Utf8BytesForDisplayChars(remaining.c_str(), chars_fit);
    if (bytes > remaining.size())
      bytes = remaining.size();

    std::string line_text = remaining.substr(0, bytes);

    if ((int)lines.size() == max_lines - 1 && bytes < remaining.size()) {
      if (chars_fit > 4) {
        size_t text_bytes = text_unicode_utils::Utf8BytesForDisplayChars(
            remaining.c_str(), chars_fit - 3);
        line_text = remaining.substr(0, text_bytes) + "...";
      }
    }

    lines.push_back(line_text);
    remaining = remaining.substr(bytes);
  }

  return lines;
}

}

BookmarkMenu::BookmarkMenu(App *_app) : PagedListMenu(_app, "bookmarks") {}

BookmarkMenu::~BookmarkMenu() {}

void BookmarkMenu::BuildEntries(std::vector<std::string> &labels,
                                std::vector<u16> &pages) {
  Book *book = app ? app->GetCurrentBook() : NULL;
  if (!book)
    return;

  std::list<u16> &bookmarks = book->GetBookmarks();
  labels.reserve(bookmarks.size());
  pages.reserve(bookmarks.size());

  const int kPreviewWidth = 220;
  const int kMaxPreviewLines = 2;
  const size_t kPreviewChars = 120;

  for (auto pg : bookmarks) {
    char label[64];
    snprintf(label, sizeof(label), "Page %d", pg + 1);
    std::string full_label = label;

    Page *page = book->GetPage(pg);
    if (page) {
      std::string preview = ExtractPagePreview(page, kPreviewChars);
      if (!preview.empty()) {
        std::vector<std::string> lines = WrapTextToLines(
            app->ts.get(), preview, kPreviewWidth, kMaxPreviewLines);
        for (const auto &line : lines) {
          full_label += "\n" + line;
        }
      }
    }

    labels.push_back(full_label);
    pages.push_back(pg);
  }
}

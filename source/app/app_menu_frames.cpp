/*
    3dslibris - app_menu_frames.cpp
    Extracted from app.cpp. Holds per-frame entry points for the modal
    overlay menus (font picker, bookmarks, chapters/TOC).

    No behavior change — pure code motion.
*/

#include "app/app.h"

#include <3ds.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>
#include <vector>

#include "book/book.h"
#include "book/book_xml_css_style_utils.h"
#include "formats/common/html_entity_utils.h"
#include "menus/bookmark_menu.h"
#include "menus/chapter_menu.h"
#include "shared/string_utils.h"
#include "shared/text_unicode_utils.h"
#include "settings/font.h"
#include "shared/debug_log.h"
#include "ui/screen_layout_constants.h"
#include "ui/text.h"

namespace {

static const char *FormatLabel(format_t format) {
  switch (format) {
  case FORMAT_EPUB:
    return "EPUB";
  case FORMAT_PDF:
    return "PDF";
  case FORMAT_CBZ:
    return "CBZ";
  case FORMAT_XHTML:
    return "XHTML";
  case FORMAT_UNDEF:
  default:
    return "Unknown";
  }
}

static std::vector<std::string> WrapToWidth(Text *ts, const std::string &text,
                                            int max_width, int max_lines) {
  std::vector<std::string> out;
  if (!ts || max_width <= 0 || max_lines <= 0) {
    out.push_back(text);
    return out;
  }

  std::string remaining = text.empty() ? std::string("-") : text;
  while (!remaining.empty() && (int)out.size() < max_lines) {
    while (!remaining.empty() && isspace((unsigned char)remaining[0]))
      remaining.erase(remaining.begin());
    if (remaining.empty())
      break;

    size_t newline_pos = remaining.find('\n');
    std::string chunk =
        (newline_pos == std::string::npos) ? remaining : remaining.substr(0, newline_pos);
    if (chunk.empty()) {
      out.push_back(std::string());
      if (newline_pos == std::string::npos)
        break;
      remaining.erase(0, newline_pos + 1);
      continue;
    }

    u8 chars_fit =
        ts->GetCharCountInsideWidth(chunk.c_str(), TEXT_STYLE_BROWSER,
                                    (u8)max_width);
    if (chars_fit == 0)
      chars_fit = 1;

    size_t bytes =
        text_unicode_utils::Utf8BytesForDisplayChars(chunk.c_str(), chars_fit);
    if (bytes == 0 || bytes > chunk.size())
      bytes = chunk.size();

    size_t split = bytes;
    if (split < chunk.size()) {
      size_t ws = split;
      while (ws > 0 && !isspace((unsigned char)chunk[ws - 1]))
        ws--;
      if (ws > 0)
        split = ws;
    }

    std::string line = chunk.substr(0, split);
    while (!line.empty() && isspace((unsigned char)line[line.size() - 1]))
      line.erase(line.size() - 1);
    out.push_back(line.empty() ? std::string("-") : line);

    if (newline_pos != std::string::npos && split >= chunk.size()) {
      remaining.erase(0, newline_pos + 1);
    } else {
      remaining.erase(0, split);
    }
  }

  if (out.empty())
    out.push_back("-");
  return out;
}

static std::vector<std::string> WrapToWidthWithStyle(
    Text *ts, const std::string &text, int max_width, int max_lines, u8 style,
    bool dash_if_empty) {
  std::vector<std::string> out;
  if (!ts || max_width <= 0 || max_lines <= 0) {
    out.push_back(text);
    return out;
  }

  std::string remaining =
      text.empty() ? (dash_if_empty ? std::string("-") : std::string()) : text;
  while (!remaining.empty() && (int)out.size() < max_lines) {
    while (!remaining.empty() && isspace((unsigned char)remaining[0]))
      remaining.erase(remaining.begin());
    if (remaining.empty())
      break;

    size_t newline_pos = remaining.find('\n');
    std::string chunk =
        (newline_pos == std::string::npos) ? remaining : remaining.substr(0, newline_pos);
    if (chunk.empty()) {
      out.push_back(std::string());
      if (newline_pos == std::string::npos)
        break;
      remaining.erase(0, newline_pos + 1);
      continue;
    }

    u8 chars_fit =
        ts->GetCharCountInsideWidth(chunk.c_str(), style,
                                    (u8)max_width);
    if (chars_fit == 0)
      chars_fit = 1;

    size_t bytes =
        text_unicode_utils::Utf8BytesForDisplayChars(chunk.c_str(), chars_fit);
    if (bytes == 0 || bytes > chunk.size())
      bytes = chunk.size();

    size_t split = bytes;
    if (split < chunk.size()) {
      size_t ws = split;
      while (ws > 0 && !isspace((unsigned char)chunk[ws - 1]))
        ws--;
      if (ws > 0)
        split = ws;
    }

    std::string line = chunk.substr(0, split);
    while (!line.empty() && isspace((unsigned char)line[line.size() - 1]))
      line.erase(line.size() - 1);
    if (line.empty() && dash_if_empty)
      out.push_back("-");
    else
      out.push_back(line);

    if (newline_pos != std::string::npos && split >= chunk.size()) {
      remaining.erase(0, newline_pos + 1);
    } else {
      remaining.erase(0, split);
    }
  }

  if (out.empty() && dash_if_empty)
    out.push_back("-");
  return out;
}

static std::string FormatTimestampLabel(uint32_t unix_time) {
  if (unix_time == 0)
    return "never";
  time_t raw = (time_t)unix_time;
  struct tm *local = localtime(&raw);
  if (!local)
    return "unknown";
  char out[40];
  snprintf(out, sizeof(out), "%04d-%02d-%02d %02d:%02d",
           local->tm_year + 1900, local->tm_mon + 1, local->tm_mday,
           local->tm_hour, local->tm_min);
  return std::string(out);
}

static const char *DisplayOrDash(const std::string &value) {
  return value.empty() ? "-" : value.c_str();
}

static bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static std::string TrimAscii(const std::string &in) {
  size_t start = 0;
  while (start < in.size() && IsAsciiSpace(in[start]))
    start++;
  size_t end = in.size();
  while (end > start && IsAsciiSpace(in[end - 1]))
    end--;
  return in.substr(start, end - start);
}

static bool IsAsciiWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static bool IsSelfClosingTag(const std::string &raw) {
  size_t end = raw.size();
  while (end > 0 && IsAsciiSpace(raw[end - 1]))
    end--;
  return end > 0 && raw[end - 1] == '/';
}

static std::string HtmlTagNameLower(const std::string &raw) {
  if (raw.empty())
    return std::string();
  size_t i = 0;
  while (i < raw.size() && IsAsciiSpace(raw[i]))
    i++;
  if (i < raw.size() && raw[i] == '/')
    i++;
  while (i < raw.size() && IsAsciiSpace(raw[i]))
    i++;
  size_t j = i;
  while (j < raw.size() && raw[j] != '>' && !IsAsciiSpace(raw[j]) &&
         raw[j] != '/')
    j++;
  std::string name = raw.substr(i, j - i);
  for (size_t k = 0; k < name.size(); k++) {
    if (name[k] >= 'A' && name[k] <= 'Z')
      name[k] = (char)(name[k] - 'A' + 'a');
  }
  return name;
}

static std::string ExtractInlineStyleAttr(const std::string &tag_raw) {
  if (tag_raw.empty())
    return std::string();
  const std::string lc = ToLowerAscii(tag_raw);
  size_t pos = 0;
  while (true) {
    pos = lc.find("style", pos);
    if (pos == std::string::npos)
      return std::string();
    if (pos > 0 && IsAsciiWordChar(lc[pos - 1])) {
      pos++;
      continue;
    }
    size_t p = pos + 5;
    while (p < lc.size() && IsAsciiSpace(lc[p]))
      p++;
    if (p >= lc.size() || lc[p] != '=') {
      pos++;
      continue;
    }
    p++;
    while (p < tag_raw.size() && IsAsciiSpace(tag_raw[p]))
      p++;
    if (p >= tag_raw.size())
      return std::string();

    if (tag_raw[p] == '"' || tag_raw[p] == '\'') {
      const char quote = tag_raw[p++];
      size_t end = p;
      while (end < tag_raw.size() && tag_raw[end] != quote)
        end++;
      return tag_raw.substr(p, end - p);
    }

    size_t end = p;
    while (end < tag_raw.size() && !IsAsciiSpace(tag_raw[end]) &&
           tag_raw[end] != '>')
      end++;
    return tag_raw.substr(p, end - p);
  }
}

static std::string DecodeHtmlEntitiesInText(const std::string &text) {
  if (text.empty())
    return std::string();
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '&') {
      size_t semi = text.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 24) {
        std::string entity = text.substr(i, semi - i + 1);
        std::string decoded;
        if (html_entity_utils::DecodeHtmlEntityUtf8(entity, &decoded)) {
          out += decoded;
          i = semi + 1;
          continue;
        }
      }
    }

    char c = text[i];
    if (c == '\r' || c == '\t')
      c = ' ';
    out.push_back(c);
    i++;
  }
  return out;
}

static std::string ApplyAsciiTextTransform(
    const std::string &in,
    book_xml_css_style_utils::TextTransform transform) {
  if (in.empty() || transform == book_xml_css_style_utils::TextTransform::None)
    return in;

  std::string out = in;
  if (transform == book_xml_css_style_utils::TextTransform::Uppercase) {
    for (size_t i = 0; i < out.size(); i++) {
      if (out[i] >= 'a' && out[i] <= 'z')
        out[i] = (char)(out[i] - 'a' + 'A');
    }
    return out;
  }

  if (transform == book_xml_css_style_utils::TextTransform::Lowercase) {
    for (size_t i = 0; i < out.size(); i++) {
      if (out[i] >= 'A' && out[i] <= 'Z')
        out[i] = (char)(out[i] - 'A' + 'a');
    }
    return out;
  }

  bool start_word = true;
  for (size_t i = 0; i < out.size(); i++) {
    char c = out[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      if (start_word && c >= 'a' && c <= 'z')
        out[i] = (char)(c - 'a' + 'A');
      else if (!start_word && c >= 'A' && c <= 'Z')
        out[i] = (char)(c - 'A' + 'a');
      start_word = false;
    } else {
      start_word = (c == ' ' || c == '\n' || c == '\t' || c == '-' ||
                    c == '_' || c == '/' || c == '.');
    }
  }
  return out;
}

static std::string CollapseExcessBlankLines(const std::string &in) {
  if (in.empty())
    return in;
  std::string out;
  out.reserve(in.size());
  int newline_run = 0;
  for (size_t i = 0; i < in.size(); i++) {
    char c = in[i];
    if (c == '\n') {
      if (newline_run < 2)
        out.push_back(c);
      newline_run++;
    } else {
      newline_run = 0;
      out.push_back(c);
    }
  }
  return out;
}

static void AppendSingleNewline(std::string *out) {
  if (!out)
    return;
  if (out->empty() || (*out)[out->size() - 1] != '\n')
    out->push_back('\n');
}

static void AppendParagraphBreak(std::string *out) {
  if (!out)
    return;
  if (out->empty()) {
    out->push_back('\n');
    return;
  }
  if ((*out)[out->size() - 1] != '\n')
    out->push_back('\n');
  if (out->size() < 2 || (*out)[out->size() - 2] != '\n')
    out->push_back('\n');
}

struct StyledRenderLine {
  std::string text;
  u8 style;
};

static u8 ResolveUiTextStyle(bool bold, bool italic) {
  if (bold && italic)
    return TEXT_STYLE_BOLDITALIC;
  if (bold)
    return TEXT_STYLE_BOLD;
  if (italic)
    return TEXT_STYLE_ITALIC;
  return TEXT_STYLE_BROWSER;
}

static std::vector<StyledRenderLine>
BuildNormalizedDescriptionRenderLines(Text *ts, const std::string &raw,
                                      int max_width) {
  std::vector<StyledRenderLine> wrapped_lines;
  if (raw.empty())
    return wrapped_lines;

  struct StyleState {
    bool bold;
    bool italic;
    bool underline;
    book_xml_css_style_utils::TextTransform text_transform;
    book_xml_css_style_utils::WhiteSpaceMode white_space;

    StyleState()
        : bold(false), italic(false), underline(false),
          text_transform(book_xml_css_style_utils::TextTransform::None),
          white_space(book_xml_css_style_utils::WhiteSpaceMode::Normal) {}
  };

  struct TagFrame {
    std::string name;
    StyleState previous;
  };

  struct RawLine {
    std::string text;
    bool bold;
    bool italic;

    RawLine() : text(), bold(false), italic(false) {}
    RawLine(const std::string &t, bool b, bool i) : text(t), bold(b), italic(i) {}
  };

  std::vector<RawLine> raw_lines;
  std::string current_line;
  current_line.reserve(raw.size() / 4 + 16);
  bool current_line_bold = false;
  bool current_line_italic = false;

  auto flush_line = [&]() {
    const std::string trimmed = TrimAscii(current_line);
    if (trimmed.empty()) {
      if (raw_lines.empty() || raw_lines.back().text.empty()) {
        current_line.clear();
        current_line_bold = false;
        current_line_italic = false;
        return;
      }
      raw_lines.push_back(RawLine(std::string(), false, false));
    } else {
      raw_lines.push_back(RawLine(trimmed, current_line_bold, current_line_italic));
    }
    current_line.clear();
    current_line_bold = false;
    current_line_italic = false;
  };

  auto paragraph_break = [&]() {
    flush_line();
    if (raw_lines.empty() || raw_lines.back().text.empty())
      return;
    raw_lines.push_back(RawLine(std::string(), false, false));
  };

  StyleState current;
  std::vector<TagFrame> tag_stack;

  for (size_t i = 0; i < raw.size();) {
    const char c = raw[i];
    if (c == '<') {
      size_t gt = raw.find('>', i + 1);
      if (gt == std::string::npos)
        break;

      std::string tag = raw.substr(i + 1, gt - i - 1);
      tag = TrimAscii(tag);
      const bool self_closing = IsSelfClosingTag(tag);
      bool closing = false;
      if (!tag.empty() && tag[0] == '/') {
        closing = true;
        tag = TrimAscii(tag.substr(1));
      }
      const std::string name = HtmlTagNameLower(tag);

      if (name == "br") {
        flush_line();
      } else if (name == "p" || name == "div" || name == "section" ||
                 name == "article") {
        if (!closing)
          paragraph_break();
      } else if (name == "li") {
        if (!closing) {
          flush_line();
          current_line += "- ";
        } else {
          flush_line();
        }
      } else if (name == "h1" || name == "h2" || name == "h3" ||
                 name == "h4" || name == "h5" || name == "h6") {
        if (!closing)
          paragraph_break();
      }

      if (!name.empty()) {
        if (closing) {
          int match = -1;
          for (int j = (int)tag_stack.size() - 1; j >= 0; --j) {
            if (tag_stack[(size_t)j].name == name) {
              match = j;
              break;
            }
          }
          if (match >= 0) {
            while ((int)tag_stack.size() > match) {
              TagFrame frame = tag_stack.back();
              tag_stack.pop_back();
              current = frame.previous;
            }
          }
        } else {
          StyleState next = current;
          if (name == "b" || name == "strong")
            next.bold = true;
          if (name == "i" || name == "em")
            next.italic = true;
          if (name == "u")
            next.underline = true;

          const std::string inline_style = ExtractInlineStyleAttr(tag);
          if (!inline_style.empty()) {
            book_xml_css_style_utils::InlineStyleFlags flags{};
            book_xml_css_style_utils::ParseInlineStyleFlags(
                inline_style.c_str(), &flags);
            if (flags.reset_bold)
              next.bold = false;
            if (flags.reset_italic)
              next.italic = false;
            if (flags.no_underline)
              next.underline = false;
            if (flags.bold)
              next.bold = true;
            if (flags.italic)
              next.italic = true;
            if (flags.underline)
              next.underline = true;

            book_xml_css_style_utils::WhiteSpaceMode ws_mode;
            if (book_xml_css_style_utils::TryParseWhiteSpace(
                    inline_style.c_str(), &ws_mode)) {
              next.white_space = ws_mode;
            }

            const book_xml_css_style_utils::TextTransform tt =
                book_xml_css_style_utils::ParseTextTransform(
                    inline_style.c_str());
            if (tt != book_xml_css_style_utils::TextTransform::None)
              next.text_transform = tt;
          }

          TagFrame frame;
          frame.name = name;
          frame.previous = current;

          if (!self_closing)
            tag_stack.push_back(frame);
          current = next;

          if (self_closing) {
            current = frame.previous;
          }
        }
      }

      i = gt + 1;
      continue;
    }

    size_t next_tag = raw.find('<', i);
    const std::string chunk =
        raw.substr(i, (next_tag == std::string::npos) ? std::string::npos
                                                       : next_tag - i);
    std::string decoded = DecodeHtmlEntitiesInText(chunk);
    decoded = book_xml_css_style_utils::NormalizeWhiteSpaceText(
        decoded.c_str(), decoded.size(), current.white_space);
    decoded = ApplyAsciiTextTransform(decoded, current.text_transform);

    for (size_t k = 0; k < decoded.size(); ++k) {
      const char ch = decoded[k];
      if (ch == '\n') {
        flush_line();
        continue;
      }
      current_line.push_back(ch);
      if (!IsAsciiSpace(ch)) {
        if (current.bold)
          current_line_bold = true;
        if (current.italic)
          current_line_italic = true;
      }
    }

    if (next_tag == std::string::npos)
      break;
    i = next_tag;
  }

  if (!current_line.empty() || raw_lines.empty())
    flush_line();

  if (raw_lines.empty())
    raw_lines.push_back(RawLine("-", false, false));

  for (size_t i = 0; i < raw_lines.size(); i++) {
    const RawLine &line = raw_lines[i];
    const u8 style = ResolveUiTextStyle(line.bold, line.italic);
    if (line.text.empty()) {
      wrapped_lines.push_back(StyledRenderLine{"", TEXT_STYLE_BROWSER});
      continue;
    }

    std::vector<std::string> chunks =
        WrapToWidthWithStyle(ts, line.text, max_width, 512, style, false);
    if (chunks.empty())
      chunks.push_back(line.text);
    for (size_t j = 0; j < chunks.size(); j++) {
      wrapped_lines.push_back(StyledRenderLine{chunks[j], style});
    }
  }

  if (wrapped_lines.empty())
    wrapped_lines.push_back(StyledRenderLine{"-", TEXT_STYLE_BROWSER});
  return wrapped_lines;
}

} // namespace

void App::RunFontMenuFrame(u32 keys)
{
#ifdef DSLIBRIS_DEBUG
  static int s_font_frame_budget = 48;
  if (s_font_frame_budget > 0)
  {
    DBG_LOGF(this,
             "FONT frame keys=0x%08lx dirty=%d screen=%p right=%p left=%p ts_dirty=%d",
             (unsigned long)keys, fontmenu->isDirty() ? 1 : 0,
             (void *)ts->GetScreen(), (void *)ts->screenright,
             (void *)ts->screenleft, ts->HasDirtyScreens() ? 1 : 0);
    s_font_frame_budget--;
  }
#endif
  // Ensure first entry into font submenu is visible before any new key edge.
  if (fontmenu->isDirty())
  {
    ts->SetScreen(ts->screenright);
    fontmenu->draw();
    // Defensive: ensure framebuffer conversion sees this submenu redraw.
    ts->MarkScreenDirty(ts->screenright);
#ifdef DSLIBRIS_DEBUG
    static int s_font_predraw_budget = 16;
    if (s_font_predraw_budget > 0)
    {
      DBG_LOGF(this,
               "FONT frame pre-draw done ts_dirty=%d screen=%p right=%p left=%p",
               ts->HasDirtyScreens() ? 1 : 0, (void *)ts->GetScreen(),
               (void *)ts->screenright, (void *)ts->screenleft);
      s_font_predraw_budget--;
    }
#endif
  }

  if (keys == 0)
    return;

  fontmenu->HandleInput(keys);
  if (fontmenu->isDirty())
  {
    ts->SetScreen(ts->screenright);
    fontmenu->draw();
    ts->MarkScreenDirty(ts->screenright);
#ifdef DSLIBRIS_DEBUG
    static int s_font_draw_after_input_budget = 24;
    if (s_font_draw_after_input_budget > 0)
    {
      DBG_LOGF(this, "FONT frame draw-after-input ts_dirty=%d",
               ts->HasDirtyScreens() ? 1 : 0);
      s_font_draw_after_input_budget--;
    }
#endif
  }
}

void App::RunBookmarksMenuFrame(u32 keys)
{
  bookmarkmenu->HandleInput(keys);
  if (bookmarkmenu->IsDirty())
    bookmarkmenu->Draw();
}

void App::RunChaptersMenuFrame(u32 keys)
{
#ifdef DSLIBRIS_DEBUG
  static int s_chapters_frame_budget = 24;
  if (s_chapters_frame_budget > 0)
  {
    DBG_LOGF(this, "INDEX frame keys=0x%08lx dirty=%d", (unsigned long)keys,
             chaptermenu && chaptermenu->IsDirty() ? 1 : 0);
    s_chapters_frame_budget--;
  }
  static int s_chapters_input_budget = 64;
  if (s_chapters_input_budget > 0 && (keys != 0 || hidKeysHeld() != 0))
  {
    DBG_LOGF(this, "INDEX input down=0x%08lx held=0x%08lx",
             (unsigned long)keys, (unsigned long)hidKeysHeld());
    s_chapters_input_budget--;
  }
#endif
  // Draw first when invalidated so the index becomes visible even before any
  // new key edge arrives.
  if (chaptermenu->IsDirty())
  {
    chaptermenu->Draw();
    ts->MarkScreenDirty(ts->screenright);
#ifdef DSLIBRIS_DEBUG
    static int s_chapters_predraw_budget = 16;
    if (s_chapters_predraw_budget > 0)
    {
      DBG_LOG(this, "INDEX frame pre-draw");
      s_chapters_predraw_budget--;
    }
#endif
  }

  // Chapters navigation is edge-triggered (`hidKeysDown` in main loop). Avoid
  // processing idle frames in the menu handler to keep this path deterministic.
  if (keys == 0)
    return;

  chaptermenu->HandleInput(keys);
  const bool dirty_after_input = chaptermenu && chaptermenu->IsDirty();
#ifdef DSLIBRIS_DEBUG
  {
    static int s_chapters_dirty_budget = 64;
    const bool dirty_before = chaptermenu->IsDirty();
    (void)dirty_before;
    if (s_chapters_dirty_budget > 0)
    {
      DBG_LOGF(this, "INDEX frame state dirty_after_input=%d", dirty_after_input ? 1 : 0);
      s_chapters_dirty_budget--;
    }
  }
#endif
  if (dirty_after_input)
    chaptermenu->Draw();
#ifdef DSLIBRIS_DEBUG
  static int s_chapters_draw_budget = 32;
  if (s_chapters_draw_budget > 0 && dirty_after_input)
  {
    DBG_LOG(this, "INDEX frame draw");
    s_chapters_draw_budget--;
  }
#endif
}

void App::RunBookInfoFrame(u32 keys)
{
  if (keys & (KEY_B | KEY_SELECT | KEY_START | KEY_Y | KEY_A)) {
    ShowSettingsView(true);
    return;
  }

  const int row_h = ts->GetHeight() + 2;
  const int label_x = 8;
  const int value_x = 110;
  const int value_w = 236 - value_x;
  const int desc_x = 8;
  const int desc_w = 236 - desc_x;
  const int header_y = 14;
  const int body_start_y = header_y + row_h + 2;
  const int footer_text_y = screen_layout::kFooterY - row_h - 8;
  const int footer_y = footer_text_y - row_h - 4;
  const int description_text_start_y = body_start_y + row_h;
  int description_page_lines = (footer_y - description_text_start_y) / row_h;
  if (description_page_lines <= 0)
    description_page_lines = 1;

  Book *book = reader_state_.bookcurrent;
  std::vector<StyledRenderLine> wrapped_desc_lines;
  int description_pages = 1;
  if (book) {
    wrapped_desc_lines = BuildNormalizedDescriptionRenderLines(
        ts.get(), book->GetDescription(), desc_w);
    if (wrapped_desc_lines.empty())
      wrapped_desc_lines.push_back(StyledRenderLine{"-", TEXT_STYLE_BROWSER});
    description_pages =
        (int)((wrapped_desc_lines.size() + (size_t)description_page_lines - 1) /
              (size_t)description_page_lines);
    if (description_pages < 1)
      description_pages = 1;
  }

  const int kBookInfoPageCount = 2 + description_pages;
  if (nav_.book_info_page >= (u8)kBookInfoPageCount)
    nav_.book_info_page = (u8)(kBookInfoPageCount - 1);

  if ((keys & (key.left | key.l)) && nav_.book_info_page > 0) {
    nav_.book_info_page--;
    ts->MarkScreenDirty(ts->screenright);
  }
  if ((keys & (key.right | key.r)) &&
      nav_.book_info_page + 1 < kBookInfoPageCount) {
    nav_.book_info_page++;
    ts->MarkScreenDirty(ts->screenright);
  }

  if (keys & KEY_TOUCH) {
    touchPosition touch = TouchRead();
    const int x = (int)touch.px;
    const int y = (int)touch.py;
    if (buttonback.EnclosesPoint((u16)x, (u16)y)) {
      ShowSettingsView(true);
      return;
    }
    if (nav_.book_info_page > 0 &&
        buttonprev.EnclosesPoint((u16)x, (u16)y)) {
      nav_.book_info_page--;
      ts->MarkScreenDirty(ts->screenright);
    } else if (nav_.book_info_page + 1 < kBookInfoPageCount &&
               buttonnext.EnclosesPoint((u16)x, (u16)y)) {
      nav_.book_info_page++;
      ts->MarkScreenDirty(ts->screenright);
    }
  }

  if (keys == 0 && !ts->HasDirtyScreens())
    return;

  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  DrawBottomGradientBackground();

  const int saved_style = ts->GetStyle();
  ts->SetStyle(TEXT_STYLE_BROWSER);

  int y = header_y;
  ts->SetPen(8, y);
  ts->PrintString("book information");
  y = body_start_y;

  if (!book)
  {
    ts->SetPen(8, y);
    ts->PrintString("No current book");
    y += row_h;
  }
  else
  {
    const std::string title =
        (book->GetTitle() && book->GetTitle()[0]) ? book->GetTitle() : "(untitled)";
    const std::string author =
        (!book->GetAuthor().empty()) ? book->GetAuthor() : "(unknown)";
    const std::string format = FormatLabel(book->format);
    const std::string series = book->GetSeries();
    const std::string language = book->GetLanguage();

    char pages[32];
    snprintf(pages, sizeof(pages), "%u", (unsigned)book->GetPageCount());
    char chapters[32];
    snprintf(chapters, sizeof(chapters), "%u",
             (unsigned)book->GetChapters().size());
    char pos[32];
    snprintf(pos, sizeof(pos), "%d", (int)book->GetPosition() + 1);

    const std::string last_read =
        FormatTimestampLabel(book->GetLastOpenedTime());

    const std::string publisher = book->GetPublisher();
    const std::string published = book->GetPublished();
    const std::string subjects = book->GetSubjects();

    struct InfoLine {
      const char *label;
      std::string value;
      int max_lines;
    };

    const InfoLine page0_lines[] = {
      {"title", title, 2},
      {"author", author, 2},
      {"series", series, 2},
      {"language", language, 1},
      {"format", format, 1},
      {"page", pos, 1},
      {"pages", pages, 1},
      {"chapters", chapters, 1},
      {"last read", last_read, 1},
    };

    const InfoLine page1_lines[] = {
      {"publisher", publisher, 4},
      {"published", published, 2},
      {"subjects", subjects, 4},
    };

    if (nav_.book_info_page == 0 || nav_.book_info_page == 1) {
      const InfoLine *lines = nav_.book_info_page == 0 ? page0_lines : page1_lines;
      const size_t line_count =
          nav_.book_info_page == 0 ? sizeof(page0_lines) / sizeof(page0_lines[0])
                                   : sizeof(page1_lines) / sizeof(page1_lines[0]);

      for (size_t i = 0; i < line_count; i++)
      {
        if (y >= footer_y)
          break;
        std::vector<std::string> wrapped =
            WrapToWidth(ts.get(), DisplayOrDash(lines[i].value), value_w,
                        lines[i].max_lines);
        for (size_t j = 0; j < wrapped.size(); j++) {
          if (y >= footer_y)
            break;
          if (j == 0) {
            ts->SetPen(label_x, y);
            ts->PrintString(lines[i].label);
          }
          ts->SetPen(value_x, y);
          ts->PrintString(wrapped[j].c_str());
          y += row_h;
        }
      }
    } else {
      const int desc_page_index = (int)nav_.book_info_page - 2;
      const size_t start_line = (size_t)desc_page_index * (size_t)description_page_lines;
      size_t end_line = start_line + (size_t)description_page_lines;
      if (end_line > wrapped_desc_lines.size())
        end_line = wrapped_desc_lines.size();

      ts->SetPen(desc_x, y);
      ts->PrintString("description");
      y += row_h;
      for (size_t i = start_line; i < end_line && y < footer_y; i++) {
        ts->SetPen(desc_x, y);
        if (!wrapped_desc_lines[i].text.empty()) {
          ts->PrintString(wrapped_desc_lines[i].text.c_str(),
                          wrapped_desc_lines[i].style);
        }
        y += row_h;
      }
    }
  }

  ts->SetPen(8, footer_text_y);
  char pager[24];
  snprintf(pager, sizeof(pager), "page %d/%d", (int)nav_.book_info_page + 1,
           kBookInfoPageCount);
  ts->PrintString(pager);

  buttonback.Draw(false);
  if (nav_.book_info_page > 0)
    buttonprev.Draw(false);
  if (nav_.book_info_page + 1 < kBookInfoPageCount)
    buttonnext.Draw(false);

  ts->SetStyle(saved_style);
}

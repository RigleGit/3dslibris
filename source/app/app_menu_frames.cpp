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
#include "menus/bookmark_menu.h"
#include "menus/chapter_menu.h"
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
  const int kBookInfoPageCount = 2;
  if (keys & (KEY_B | KEY_SELECT | KEY_START | KEY_Y | KEY_A)) {
    ShowSettingsView(true);
    return;
  }

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

  int y = 14;
  const int row_h = ts->GetHeight() + 2;
  const int label_x = 8;
  const int value_x = 110;
  const int value_w = 236 - value_x;

  ts->SetPen(8, y);
  ts->PrintString("book information");
  y += row_h + 2;

  Book *book = reader_state_.bookcurrent;
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
    const std::string description = book->GetDescription();

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
        {"publisher", publisher, 3},
        {"published", published, 2},
        {"subjects", subjects, 3},
      {"description", description, 24},
    };

    const InfoLine *lines = nav_.book_info_page == 0 ? page0_lines : page1_lines;
    const size_t line_count =
        nav_.book_info_page == 0 ? sizeof(page0_lines) / sizeof(page0_lines[0])
                                 : sizeof(page1_lines) / sizeof(page1_lines[0]);

    const int footer_text_y = screen_layout::kFooterY - row_h - 8;
    const int footer_y = footer_text_y - row_h - 4;

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
  }

  const int footer_text_y = screen_layout::kFooterY - row_h - 8;
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

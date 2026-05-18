/*
    3dslibris - app_menu_frames.cpp
    Extracted from app.cpp. Holds per-frame entry points for the modal
    overlay menus (font picker, bookmarks, chapters/TOC).

    No behavior change — pure code motion.
*/

#include "app/app.h"

#include <3ds.h>

#include "menus/bookmark_menu.h"
#include "menus/chapter_menu.h"
#include "settings/font.h"
#include "shared/debug_log.h"
#include "ui/text.h"

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

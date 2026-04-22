/*
    3dslibris - paged_list_menu.cpp
    New shared menu module for the 3DS port by Rigle.

    Summary:
    - Generic paged list renderer and input handler for index/bookmarks.
    - Unified footer button layout and touch routing with robust candidates.
    - Activation/back/navigation hooks reused by specialized menu types.
*/

#include "menus/paged_list_menu.h"

#include <algorithm>
#include <stdio.h>

#include "app/app.h"
#include "book/book.h"
#include "ui/button.h"
#include "debug_log.h"
#include "book/page.h"
#include "ui/touch_utils.h"

namespace {

static const int LIST_HEADER_X = 6;
static const int LIST_HEADER_Y = 14;
static const int LIST_ROW_X = 5;
static const int LIST_ROW_Y0 = 24;
static const int LIST_ROW_W = 230;
static const int LIST_ROW_H = 34;
static const int LIST_ROW_GAP = 2;
static const int LIST_FOOTER_Y = 292;

static u16 ClampPageTarget(u16 target_page, u16 page_count) {
  if (page_count == 0)
    return 0;
  const u16 last_page = (u16)(page_count - 1);
  return target_page > last_page ? last_page : target_page;
}

} // namespace

PagedListMenu::PagedListMenu(App *_app, const char *title) : Menu(_app) {
  pagesize = 7;
  header_title = title ? title : "";
  wait_input_release = false;
  wait_input_release_started_ms = 0;
}

PagedListMenu::~PagedListMenu() {
  for (auto button : buttons) {
    delete button;
  }
  buttons.clear();
}

void PagedListMenu::LayoutFooterButtons() {
  app->buttonprev.Move(6, LIST_FOOTER_Y);
  app->buttonprev.Resize(68, 22);
  app->buttonprev.Label("prev");

  app->buttonprefs.Move(86, LIST_FOOTER_Y);
  app->buttonprefs.Resize(68, 22);
  app->buttonprefs.Label("back");

  app->buttonnext.Move(166, LIST_FOOTER_Y);
  app->buttonnext.Resize(68, 22);
  app->buttonnext.Label("next");
}

void PagedListMenu::Init() {
  for (auto button : buttons) {
    delete button;
  }
  buttons.clear();
  target_pages.clear();

  if (!app || !app->GetCurrentBook()) {
    if (app) {
      DBG_LOGF(app, "LIST init skipped title=%s app=%p book=%p",
               header_title.c_str(), (void *)app,
               (void *)(app ? app->GetCurrentBook() : NULL));
    }
    dirty = true;
    return;
  }

  std::vector<std::string> labels;
  std::vector<u16> pages;
  BuildEntries(labels, pages);

  size_t count = std::min(labels.size(), pages.size());
  target_pages.reserve(count);
  for (size_t i = 0; i < count; i++) {
    Button *b = new Button(app->ts.get());
    b->Init();
    b->Resize(LIST_ROW_W, LIST_ROW_H);
    std::string line1 = labels[i];
    std::string line2;
    std::string line3;
    size_t nl1 = line1.find('\n');
    if (nl1 != std::string::npos) {
      std::string rest = line1.substr(nl1 + 1);
      line1 = line1.substr(0, nl1);
      size_t nl2 = rest.find('\n');
      if (nl2 != std::string::npos) {
        line2 = rest.substr(0, nl2);
        line3 = rest.substr(nl2 + 1);
      } else {
        line2 = rest;
      }
    }
    b->SetLabel1(line1);
    b->SetLabel2(line2);
    b->SetLabel3(line3);

    int line_count = line3.empty() ? (line2.empty() ? 1 : 2) : 3;
    int line_height = 14;
    if (app && app->ts)
      line_height = app->ts->GetHeight();
    if (line_height < 10)
      line_height = 10;
    int button_height = LIST_ROW_H;
    if (line_count > 2)
      button_height = 8 + line_count * line_height;
    else if (line_count == 2 && (8 + 2 * line_height) > LIST_ROW_H)
      button_height = 8 + 2 * line_height;
    b->Resize(LIST_ROW_W, button_height);
    buttons.push_back(b);
    target_pages.push_back(pages[i]);
  }

  page_sizes.clear();
  int current_y = LIST_ROW_Y0;
  u8 items_on_page = 0;
  for (size_t i = 0; i < buttons.size(); i++) {
    int h = buttons[i]->GetHeight();
    if (items_on_page > 0 && current_y + LIST_ROW_GAP + h > LIST_FOOTER_Y) {
      page_sizes.push_back(items_on_page);
      current_y = LIST_ROW_Y0;
      items_on_page = 0;
    } else if (items_on_page > 0) {
      current_y += LIST_ROW_GAP;
    }
    buttons[i]->Move(LIST_ROW_X, current_y);
    current_y += h;
    items_on_page++;
  }
  if (items_on_page > 0)
    page_sizes.push_back(items_on_page);

  selected = 0;
  page = 0;
  UpdatePageSize();
  // Avoid immediate accidental activation from the touch/key used to open menu.
  wait_input_release = true;
  wait_input_release_started_ms = osGetTime();
  dirty = true;
  DBG_LOGF(app, "LIST init title=%s entries=%u pages=%u", header_title.c_str(),
           (unsigned)buttons.size(), (unsigned)GetPageCount());
}

void PagedListMenu::Draw() {
#ifdef DSLIBRIS_DEBUG
  static int s_list_draw_begin_budget = 24;
  if (app && s_list_draw_begin_budget > 0) {
    const u16 before0 = app->ts->screenright[0];
    const u16 before1 =
        app->ts->screenright[(size_t)10 * (size_t)app->ts->display.height + 10];
    DBG_LOGF(app,
             "LIST draw begin title=%s dirty=%d page=%u sel=%u before0=%04x before1=%04x",
             header_title.c_str(), dirty ? 1 : 0, (unsigned)GetCurrentPage(),
             (unsigned)selected, (unsigned)before0, (unsigned)before1);
    s_list_draw_begin_budget--;
  }
#endif
  int savedColorMode = app->ts->GetColorMode();
  app->ts->SetScreen(app->ts->screenright);
  app->ts->ClearScreen();
  app->DrawBottomGradientBackground();

  app->ts->SetPen(LIST_HEADER_X, LIST_HEADER_Y);
  app->ts->PrintString(header_title.c_str());

  u8 start = GetPageStart(page);
  u8 end = std::min<u8>((u8)buttons.size(), (u8)(start + GetPageSize(page)));
#ifdef DSLIBRIS_DEBUG
  static int s_draw_trace_budget = 48;
  if (app && s_draw_trace_budget > 0) {
    DBG_LOGF(app,
             "LIST draw title=%s entries=%u page=%u/%u sel=%u range=[%u,%u)",
             header_title.c_str(), (unsigned)buttons.size(),
             (unsigned)GetCurrentPage(), (unsigned)GetPageCount(),
             (unsigned)selected, (unsigned)start, (unsigned)end);
    s_draw_trace_budget--;
  }
#endif
  for (u8 i = start; i < end; i++) {
    buttons[i]->Draw(i == selected);
  }

  char label[32];
  u8 total_pages = page_sizes.empty() ? GetPageCount() : (u8)page_sizes.size();
  snprintf(label, sizeof(label), "Pg %d/%d", GetCurrentPage(), total_pages);
  app->ts->SetPen(6, 282);
  app->ts->PrintString(label);

  LayoutFooterButtons();
  if (page > 0)
    app->buttonprev.Draw();
  app->buttonprefs.Draw();
  if (page < GetPageCount() - 1)
    app->buttonnext.Draw();

  app->ts->SetColorMode(savedColorMode);
  dirty = false;
#ifdef DSLIBRIS_DEBUG
  static int s_list_draw_end_budget = 24;
  if (app && s_list_draw_end_budget > 0) {
    const u16 after0 = app->ts->screenright[0];
    const u16 after1 =
        app->ts->screenright[(size_t)10 * (size_t)app->ts->display.height + 10];
    DBG_LOGF(app,
             "LIST draw end title=%s dirty=%d page=%u sel=%u after0=%04x after1=%04x",
             header_title.c_str(), dirty ? 1 : 0, (unsigned)GetCurrentPage(),
             (unsigned)selected, (unsigned)after0, (unsigned)after1);
    s_list_draw_end_budget--;
  }
#endif
}

void PagedListMenu::HandleInput(u32 keys) {
  const u32 release_mask = KEY_TOUCH | KEY_A | KEY_B | KEY_START | KEY_SELECT |
                           KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L |
                           KEY_R | KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT |
                           KEY_CPAD_RIGHT;
  if (wait_input_release) {
    const u32 held = hidKeysHeld() & release_mask;
    if (held) {
      // Some devices can report sticky held bits after view transitions.
      // Do not deadlock this menu waiting forever for a perfect release.
      const u64 elapsed = osGetTime() - wait_input_release_started_ms;
#ifdef DSLIBRIS_DEBUG
      static int s_wait_trace_budget = 32;
      if (app && s_wait_trace_budget > 0) {
        DBG_LOGF(app, "LIST waiting title=%s held=0x%08lx elapsed=%llums",
                 header_title.c_str(), (unsigned long)held,
                 (unsigned long long)elapsed);
        s_wait_trace_budget--;
      }
#endif
      if (elapsed < 300)
        return;
      if (app) {
        DBG_LOGF(app, "LIST release-timeout title=%s held=0x%08lx elapsed=%llums",
                 header_title.c_str(), (unsigned long)held,
                 (unsigned long long)elapsed);
      }
    }
    if (app) {
      DBG_LOGF(app, "LIST release-ok title=%s held=0x%08lx",
               header_title.c_str(), (unsigned long)held);
    }
    wait_input_release = false;
    wait_input_release_started_ms = 0;
    return;
  }

  auto key = app->key;
  const u32 non_touch_keys = keys & ~KEY_TOUCH;
#ifdef DSLIBRIS_DEBUG
  static int s_input_trace_budget = 128;
  if (app && s_input_trace_budget > 0 && (keys != 0 || hidKeysHeld() != 0)) {
    DBG_LOGF(app,
             "LIST input title=%s keys=0x%08lx non_touch=0x%08lx held=0x%08lx sel=%u/%u page=%u/%u dirty=%d wait=%d",
             header_title.c_str(), (unsigned long)keys,
             (unsigned long)non_touch_keys, (unsigned long)hidKeysHeld(),
             (unsigned)selected, (unsigned)buttons.size(),
             (unsigned)GetCurrentPage(), (unsigned)GetPageCount(),
             dirty ? 1 : 0, wait_input_release ? 1 : 0);
    s_input_trace_budget--;
  }
#endif

  if (non_touch_keys & KEY_A) {
    ActivateSelected();
  } else if (non_touch_keys & (KEY_B | KEY_START | KEY_SELECT)) {
    Back();
  } else if (non_touch_keys &
             (key.down | KEY_DOWN | key.right | KEY_RIGHT | KEY_CPAD_DOWN |
              KEY_CPAD_RIGHT)) {
    SelectNext();
  } else if (non_touch_keys &
             (key.up | KEY_UP | key.left | KEY_LEFT | KEY_CPAD_UP |
              KEY_CPAD_LEFT)) {
    SelectPrevious();
  } else if (non_touch_keys & (key.r | KEY_R)) {
    NextPage();
  } else if (non_touch_keys & (key.l | KEY_L)) {
    PreviousPage();
  } else if (keys & KEY_TOUCH) {
    HandleTouchInput();
  }
}

void PagedListMenu::SelectNext() {
  if (buttons.empty())
    return;
  if ((size_t)selected + 1 < buttons.size()) {
    selected++;
    page = GetPageForIndex(selected);
    UpdatePageSize();
    dirty = true;
#ifdef DSLIBRIS_DEBUG
    if (app) {
      DBG_LOGF(app, "LIST nav title=%s action=next sel=%u page=%u",
               header_title.c_str(), (unsigned)selected,
               (unsigned)GetCurrentPage());
    }
#endif
  }
}

void PagedListMenu::SelectPrevious() {
  if (buttons.empty())
    return;
  if (selected > 0) {
    selected--;
    page = GetPageForIndex(selected);
    UpdatePageSize();
    dirty = true;
#ifdef DSLIBRIS_DEBUG
    if (app) {
      DBG_LOGF(app, "LIST nav title=%s action=prev sel=%u page=%u",
               header_title.c_str(), (unsigned)selected,
               (unsigned)GetCurrentPage());
    }
#endif
  }
}

void PagedListMenu::NextPage() {
  if (buttons.empty())
    return;
  if (page < GetPageCount() - 1) {
    page++;
    UpdatePageSize();
    u8 next_selected = GetPageStart(page);
    selected = (next_selected < buttons.size()) ? next_selected
                                                : (u8)(buttons.size() - 1);
    dirty = true;
#ifdef DSLIBRIS_DEBUG
    if (app) {
      DBG_LOGF(app, "LIST nav title=%s action=next_page sel=%u page=%u",
               header_title.c_str(), (unsigned)selected,
               (unsigned)GetCurrentPage());
    }
#endif
  }
}

void PagedListMenu::PreviousPage() {
  if (buttons.empty())
    return;
  if (page > 0) {
    page--;
    UpdatePageSize();
    selected = GetPageStart(page);
    dirty = true;
#ifdef DSLIBRIS_DEBUG
    if (app) {
      DBG_LOGF(app, "LIST nav title=%s action=prev_page sel=%u page=%u",
               header_title.c_str(), (unsigned)selected,
               (unsigned)GetCurrentPage());
    }
#endif
  }
}

void PagedListMenu::ActivateSelected() {
  Book *book = app ? app->GetCurrentBook() : NULL;
  if (!book || buttons.empty() || selected >= target_pages.size()) {
    if (app) {
      DBG_LOGF(app,
               "LIST activate skipped title=%s book=%p buttons=%u sel=%u targets=%u",
               header_title.c_str(), (void *)book, (unsigned)buttons.size(),
               (unsigned)selected, (unsigned)target_pages.size());
    }
    return;
  }

  u16 target_page = target_pages[selected];
  const bool resolved = ResolveTargetPage(selected, &target_page);
  if (!resolved) {
    if (app) {
      DBG_LOGF(app, "LIST resolve failed title=%s sel=%u targets=%u",
               header_title.c_str(), (unsigned)selected,
               (unsigned)target_pages.size());
    }
    return;
  }

  if (app) {
    DBG_LOGF(app, "LIST activate title=%s sel=%u target=%u cur=%u",
             header_title.c_str(), (unsigned)selected, (unsigned)target_page,
             (unsigned)book->GetPosition());
  }
  const u16 page_count = book->GetPageCount();
  if (page_count == 0)
    return;
  const u16 unclamped_target = target_page;
  target_page = ClampPageTarget(target_page, page_count);
  if (app) {
    DBG_LOGF(app, "LIST target title=%s raw=%u clamped=%u page_count=%u",
             header_title.c_str(), (unsigned)unclamped_target,
             (unsigned)target_page, (unsigned)page_count);
  }
  book->SetPosition(target_page);
  app->ShowCurrentBookView();
  book->DrawCurrentView(app->ts.get());
  app->RequestStatusRedraw();
}

bool PagedListMenu::ResolveTargetPage(u8 index, u16 *page_out) {
  if (!page_out)
    return false;
  if (index >= target_pages.size())
    return false;
  *page_out = target_pages[index];
  return true;
}

u8 PagedListMenu::GetPageStart(u8 page_index) const {
  u8 start = 0;
  for (u8 i = 0; i < page_index && i < page_sizes.size(); i++)
    start += page_sizes[i];
  return start;
}

u8 PagedListMenu::GetPageSize(u8 page_index) const {
  if (page_index < page_sizes.size())
    return page_sizes[page_index];
  return pagesize;
}

u8 PagedListMenu::GetPageForIndex(u8 index) const {
  u8 start = 0;
  for (u8 i = 0; i < page_sizes.size(); i++) {
    if (index < start + page_sizes[i])
      return i;
    start += page_sizes[i];
  }
  return 0;
}

void PagedListMenu::UpdatePageSize() {
  if (page < page_sizes.size())
    pagesize = page_sizes[page];
}

u8 PagedListMenu::GetCurrentPage() const {
  return page + 1;
}

u8 PagedListMenu::GetPageCount() const {
  if (!page_sizes.empty())
    return (u8)page_sizes.size();
  return Menu::GetPageCount();
}

void PagedListMenu::SelectItem(u8 index) {
  if (index >= buttons.size())
    return;
  selected = index;
  page = GetPageForIndex(index);
  UpdatePageSize();
  dirty = true;
}

void PagedListMenu::Back() {
#ifdef DSLIBRIS_DEBUG
  if (app) {
    DBG_LOGF(app, "LIST back title=%s from_book_ctx=%d", header_title.c_str(),
             app->IsBookSettingsContext() ? 1 : 0);
  }
#endif
  if (app->IsBookSettingsContext()) {
    app->ShowSettingsView(true);
    return;
  }

  Book *book = app ? app->GetCurrentBook() : NULL;
  app->ShowCurrentBookView();
  if (book) {
    book->DrawCurrentView(app->ts.get());
  }
  app->RequestStatusRedraw();
}

void PagedListMenu::HandleTouchInput() {
  LayoutFooterButtons();
  TouchCandidates candidates;
  touch::BuildCandidates(app, &candidates);
#ifdef DSLIBRIS_DEBUG
  if (app) {
    DBG_LOGF(app, "LIST touch title=%s c0=(%d,%d)", header_title.c_str(),
             candidates.points[0].x, candidates.points[0].y);
  }
#endif

  int footerX = -1;
  touch::FirstXInBottomBand(candidates, 284, &footerX);

  for (int i = 0; i < TouchCandidates::kCount; i++) {
    int x = candidates.points[i].x;
    int y = candidates.points[i].y;
    u8 page_start = GetPageStart(page);
    u8 page_end = page_start + GetPageSize(page);
    for (u8 idx = page_start; idx < page_end; idx++) {
      if (buttons[idx]->EnclosesPoint((u16)x, (u16)y)) {
        selected = idx;
        ActivateSelected();
        return;
      }
    }
  }

  if (touch::HitsButton(candidates, &app->buttonprefs, 4)) {
#ifdef DSLIBRIS_DEBUG
    if (app) {
      DBG_LOGF(app, "LIST touch footer=back title=%s", header_title.c_str());
    }
#endif
    Back();
    return;
  }
  if (touch::HitsButton(candidates, &app->buttonnext, 4)) {
#ifdef DSLIBRIS_DEBUG
    if (app) {
      DBG_LOGF(app, "LIST touch footer=next title=%s", header_title.c_str());
    }
#endif
    NextPage();
    return;
  }
  if (touch::HitsButton(candidates, &app->buttonprev, 4)) {
#ifdef DSLIBRIS_DEBUG
    if (app) {
      DBG_LOGF(app, "LIST touch footer=prev title=%s", header_title.c_str());
    }
#endif
    PreviousPage();
    return;
  }
  if (footerX >= 0) {
    if (footerX < 80) {
      PreviousPage();
    } else if (footerX < 160) {
      Back();
    } else {
      NextPage();
    }
#ifdef DSLIBRIS_DEBUG
    if (app) {
      DBG_LOGF(app, "LIST touch footer-band title=%s x=%d", header_title.c_str(),
               footerX);
    }
#endif
    return;
  }

  u8 start = GetPageStart(page);
  u8 end = std::min<u8>((u8)buttons.size(), (u8)(start + GetPageSize(page)));
  for (u8 i = start; i < end; i++) {
    if (touch::HitsButton(candidates, buttons[i], 4)) {
      selected = i;
#ifdef DSLIBRIS_DEBUG
      if (app) {
        DBG_LOGF(app, "LIST touch button-hit title=%s sel=%u",
                 header_title.c_str(), (unsigned)selected);
      }
#endif
      ActivateSelected();
      return;
    }
  }
#ifdef DSLIBRIS_DEBUG
  if (app) {
    DBG_LOGF(app, "LIST touch no-hit title=%s", header_title.c_str());
  }
#endif
}

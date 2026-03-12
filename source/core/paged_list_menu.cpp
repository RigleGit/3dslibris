/*
    3dslibris - paged_list_menu.cpp
    New shared menu module for the 3DS port by Rigle.

    Summary:
    - Generic paged list renderer and input handler for index/bookmarks.
    - Unified footer button layout and touch routing with robust candidates.
    - Activation/back/navigation hooks reused by specialized menu types.
*/

#include "paged_list_menu.h"

#include <algorithm>
#include <stdio.h>

#include "app.h"
#include "book.h"
#include "button.h"
#include "debug_log.h"
#include "touch_utils.h"

namespace {

static const int LIST_HEADER_X = 6;
static const int LIST_HEADER_Y = 14;
static const int LIST_ROW_X = 5;
static const int LIST_ROW_Y0 = 24;
static const int LIST_ROW_W = 230;
static const int LIST_ROW_H = 34;
static const int LIST_ROW_GAP = 2;
static const int LIST_FOOTER_Y = 292;

} // namespace

PagedListMenu::PagedListMenu(App *_app, const char *title) : Menu(_app) {
  pagesize = 7;
  header_title = title ? title : "";
  wait_input_release = false;
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

  if (!app || !app->bookcurrent) {
    dirty = true;
    return;
  }

  std::vector<std::string> labels;
  std::vector<u16> pages;
  BuildEntries(labels, pages);

  size_t count = std::min(labels.size(), pages.size());
  target_pages.reserve(count);
  for (size_t i = 0; i < count; i++) {
    Button *b = new Button(app->ts);
    b->Init();
    b->Resize(LIST_ROW_W, LIST_ROW_H);
    b->Move(LIST_ROW_X, LIST_ROW_Y0 + (i % pagesize) * (LIST_ROW_H + LIST_ROW_GAP));
    std::string line1 = labels[i];
    std::string line2;
    size_t nl = line1.find('\n');
    if (nl != std::string::npos) {
      line2 = line1.substr(nl + 1);
      line1 = line1.substr(0, nl);
    }
    b->SetLabel1(line1);
    b->SetLabel2(line2);
    buttons.push_back(b);
    target_pages.push_back(pages[i]);
  }

  selected = 0;
  page = 0;
  // Avoid immediate accidental activation from the touch/key used to open menu.
  wait_input_release = true;
  dirty = true;
}

void PagedListMenu::Draw() {
  int savedColorMode = app->ts->GetColorMode();
  app->ts->SetScreen(app->ts->screenright);
  app->ts->SetColorMode(0);
  app->ts->ClearScreen();
  app->DrawBottomGradientBackground();

  app->ts->SetPen(LIST_HEADER_X, LIST_HEADER_Y);
  app->ts->PrintString(header_title.c_str());

  u8 start = page * pagesize;
  u8 end = std::min<u8>((u8)buttons.size(), (u8)(start + pagesize));
  for (u8 i = start; i < end; i++) {
    buttons[i]->Draw(i == selected);
  }

  char label[32];
  sprintf(label, "Pg %d/%d", GetCurrentPage(), GetPageCount());
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
}

void PagedListMenu::HandleInput(u32 keys) {
  const u32 release_mask = KEY_TOUCH | KEY_A | KEY_B | KEY_START | KEY_SELECT |
                           KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L |
                           KEY_R | KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT |
                           KEY_CPAD_RIGHT;
  if (wait_input_release) {
    if (hidKeysHeld() & release_mask)
      return;
    wait_input_release = false;
    return;
  }

  auto key = app->key;

  if (keys & KEY_TOUCH) {
    HandleTouchInput();
  } else if (keys & KEY_A) {
    ActivateSelected();
  } else if (keys & (KEY_B | KEY_START | KEY_SELECT)) {
    Back();
  } else if (keys & (key.down | KEY_DOWN | key.right | KEY_RIGHT |
                     KEY_CPAD_DOWN | KEY_CPAD_RIGHT)) {
    SelectNext();
  } else if (keys & (key.up | KEY_UP | key.left | KEY_LEFT | KEY_CPAD_UP |
                     KEY_CPAD_LEFT)) {
    SelectPrevious();
  } else if (keys & (key.r | KEY_R)) {
    NextPage();
  } else if (keys & (key.l | KEY_L)) {
    PreviousPage();
  }
}

void PagedListMenu::SelectNext() {
  if (buttons.empty())
    return;
  if ((size_t)selected + 1 < buttons.size()) {
    selected++;
    page = selected / pagesize;
    dirty = true;
  }
}

void PagedListMenu::SelectPrevious() {
  if (buttons.empty())
    return;
  if (selected > 0) {
    selected--;
    page = selected / pagesize;
    dirty = true;
  }
}

void PagedListMenu::NextPage() {
  if (buttons.empty())
    return;
  if (page < GetPageCount() - 1) {
    page++;
    u8 next_selected = page * pagesize;
    selected = (next_selected < buttons.size()) ? next_selected
                                                : (u8)(buttons.size() - 1);
    dirty = true;
  }
}

void PagedListMenu::PreviousPage() {
  if (buttons.empty())
    return;
  if (page > 0) {
    page--;
    selected = page * pagesize;
    dirty = true;
  }
}

void PagedListMenu::ActivateSelected() {
  if (!app || !app->bookcurrent || buttons.empty() || selected >= target_pages.size())
    return;

  u16 target_page = target_pages[selected];
  ResolveTargetPage(selected, &target_page);

  if (app) {
    char msg[128];
    snprintf(msg, sizeof(msg), "LIST activate title=%s sel=%u page=%u cur=%u",
             header_title.c_str(), (unsigned)selected, (unsigned)target_page,
             (unsigned)app->bookcurrent->GetPosition());
    DBG_LOG(app, msg);
  }
  app->bookcurrent->SetPosition(target_page);
  app->mode = APP_MODE_BOOK;
  app->bookcurrent->GetPage()->Draw(app->ts);
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

void PagedListMenu::Back() {
  if (app->IsBookSettingsContext()) {
    app->ShowSettingsView(true);
    return;
  }

  app->mode = APP_MODE_BOOK;
  if (app->bookcurrent) {
    app->bookcurrent->GetPage()->Draw(app->ts);
  }
  app->RequestStatusRedraw();
}

void PagedListMenu::HandleTouchInput() {
  LayoutFooterButtons();
  TouchCandidates candidates;
  touch::BuildCandidates(app, &candidates);

  int footerX = -1;
  touch::FirstXInBottomBand(candidates, 284, &footerX);

  // Coarse row hit-test fallback: robust even if button hitboxes drift.
  for (int i = 0; i < TouchCandidates::kCount; i++) {
    int x = candidates.points[i].x;
    int y = candidates.points[i].y;
    if (x < LIST_ROW_X || x >= LIST_ROW_X + LIST_ROW_W)
      continue;
    if (y < LIST_ROW_Y0)
      continue;
    int row = (y - LIST_ROW_Y0) / (LIST_ROW_H + LIST_ROW_GAP);
    if (row < 0 || row >= (int)pagesize)
      continue;
    int row_y = LIST_ROW_Y0 + row * (LIST_ROW_H + LIST_ROW_GAP);
    if (y >= row_y + LIST_ROW_H)
      continue;
    int idx = (int)(page * pagesize) + row;
    if (idx < 0 || idx >= (int)buttons.size())
      continue;
    selected = (u8)idx;
    ActivateSelected();
    return;
  }

  if (touch::HitsButton(candidates, &app->buttonprefs, 4)) {
    Back();
    return;
  }
  if (touch::HitsButton(candidates, &app->buttonnext, 4)) {
    NextPage();
    return;
  }
  if (touch::HitsButton(candidates, &app->buttonprev, 4)) {
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
    return;
  }

  u8 start = page * pagesize;
  u8 end = std::min<u8>((u8)buttons.size(), (u8)(start + pagesize));
  for (u8 i = start; i < end; i++) {
    if (touch::HitsButton(candidates, buttons[i], 4)) {
      selected = i;
      ActivateSelected();
      return;
    }
  }
}

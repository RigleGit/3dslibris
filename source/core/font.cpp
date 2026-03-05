#include "font.h"

#include <3ds.h>
#include <dirent.h>
#include <string>
#include <vector>

#include "app.h"
#include "button.h"

#define MIN(x, y) (x < y ? x : y)
#define MAX(x, y) (x > y ? x : y)

static const char *GetFontTargetLabel(u8 mode) {
  switch (mode) {
  case APP_MODE_PREFS_FONT_BOLD:
    return "bold";
  case APP_MODE_PREFS_FONT_ITALIC:
    return "italic";
  case APP_MODE_PREFS_FONT_BOLDITALIC:
    return "bold italic";
  case APP_MODE_PREFS_FONT:
  default:
    return "regular";
  }
}

static u8 NextFontTargetMode(u8 mode, int delta) {
  const u8 order[] = {APP_MODE_PREFS_FONT, APP_MODE_PREFS_FONT_BOLD,
                      APP_MODE_PREFS_FONT_ITALIC,
                      APP_MODE_PREFS_FONT_BOLDITALIC};
  int idx = 0;
  for (int i = 0; i < 4; i++) {
    if (order[i] == mode) {
      idx = i;
      break;
    }
  }
  idx = (idx + delta + 4) % 4;
  return order[idx];
}

static void LayoutFooterButtons(App *app) {
  app->buttonprev.Move(6, 292);
  app->buttonprev.Resize(68, 22);
  app->buttonprev.Label("prev");

  app->buttonprefs.Move(86, 292);
  app->buttonprefs.Resize(68, 22);
  app->buttonprefs.Label("back");

  app->buttonnext.Move(166, 292);
  app->buttonnext.Resize(68, 22);
  app->buttonnext.Label("next");
}

FontMenu::FontMenu(App *_app) : Menu(_app) {
  dir = app->fontdir;
  findFiles();
  for (auto &filename : files) {
    Button *b = new Button(app->ts);
    b->Init();
    b->Move(0, 20 + (buttons.size() % pagesize) * b->GetHeight());
    b->SetLabel1(std::string(filename));
    buttons.push_back(b);
  }
}

FontMenu::~FontMenu() {
  for (auto button : buttons) {
    delete button;
  }
  buttons.clear();
}

void FontMenu::findFiles() {
  DIR *dp = opendir(dir.c_str());
  if (!dp)
    return;

  struct dirent *ent;
  while ((ent = readdir(dp))) {
    // Don't try folders
    if (ent->d_type == DT_DIR)
      continue;

    char *filename = ent->d_name;
    char *c;
    for (c = filename; c != filename + strlen(filename) && *c != '.'; c++)
      ;
    if (!strcmp(".ttf", c) || !strcmp(".otf", c) || !strcmp(".ttc", c)) {
      files.push_back(std::string(filename));
    }
  }
  closedir(dp);
}

void FontMenu::handleInput() {
  // Keep input handling consistent with the rest of the 3DS app.
  u32 keys = hidKeysDown();

  // WARNING d-pad keys are in pre-rotation space!
  // TODO stop that!
  auto key = app->key;
  if (keys & KEY_B) {
    // cancel and return to settings menu
    app->ShowSettingsView();
  } else if (keys & key.up) {
    app->mode = NextFontTargetMode(app->mode, -1);
    dirty = true;
  } else if (keys & key.down) {
    app->mode = NextFontTargetMode(app->mode, +1);
    dirty = true;
  } else if (keys & (key.r | key.right)) {
    // previous font or page
    if (selected > 0) {
      if (selected == page * 7) {
        previousPage();
      } else {
        selectPrevious();
      }
    }
  } else if (keys & (key.l | key.left)) {
    // next font or page
    if (selected == page * pagesize + (pagesize - 1)) {
      nextPage();
    } else {
      selectNext();
    }
  } else if (keys & KEY_A) {
    handleButtonPress();
  } else if (keys & KEY_TOUCH) {
    handleTouchInput();
  }
}

void FontMenu::handleTouchInput() {
  LayoutFooterButtons(app);
  touchPosition coord = app->TouchRead();
  // Robust fallback zones for footer buttons (prev/back/next).
  if (coord.py >= 284) {
    if (coord.px < 80) {
      if (page > 0)
        previousPage();
    } else if (coord.px < 160) {
      app->ShowSettingsView();
    } else {
      if (page < GetPageCount() - 1)
        nextPage();
    }
    return;
  }

  auto enclosesWithSlack = [&](Button &button, int x, int y) {
    for (int dy = -4; dy <= 4; dy += 4) {
      for (int dx = -4; dx <= 4; dx += 4) {
        int tx = x + dx;
        int ty = y + dy;
        if (tx < 0 || ty < 0)
          continue;
        if (button.EnclosesPoint((u16)tx, (u16)ty))
          return true;
      }
    }
    return false;
  };

  if (enclosesWithSlack(app->buttonprefs, coord.px, coord.py)) {
    app->ShowSettingsView();
  } else if (enclosesWithSlack(app->buttonnext, coord.px, coord.py)) {
    nextPage();
  } else if (enclosesWithSlack(app->buttonprev, coord.px, coord.py)) {
    previousPage();
  } else {
    for (u8 i = page * pagesize;
         (i < buttons.size()) && (i < (page + 1) * pagesize); i++) {
      if (buttons[i]->EnclosesPoint(coord.px, coord.py)) {
        selected = i;
        handleButtonPress();
        break;
      }
    }
  }
}

void FontMenu::draw() {
  app->ts->ClearScreen();
  LayoutFooterButtons(app);
  app->ts->SetPen(6, 14);
  char header[72];
  snprintf(header, sizeof(header), "font configuration (%s)",
           GetFontTargetLabel(app->mode));
  app->ts->PrintString(header);

  for (u8 i = page * pagesize;
       (i < buttons.size()) && (i < (page + 1) * pagesize); i++) {
    buttons[i]->Draw(i == selected);
  }
  if (page > 0)
    app->buttonprev.Draw();
  app->buttonprefs.Draw();
  if (page < GetPageCount() - 1)
    app->buttonnext.Draw();
  dirty = false;
}

void FontMenu::selectNext() {
  if (selected < buttons.size() - 1) {
    selected++;
    dirty = true;
  }
}

void FontMenu::nextPage() {
  if (page + 1 * pagesize < buttons.size()) {
    page += 1;
    selected = page * pagesize;
    dirty = true;
  }
}

void FontMenu::previousPage() {
  if (page > 0) {
    page--;
    selected = page * pagesize + (pagesize - 1);
    dirty = true;
  }
}

void FontMenu::selectPrevious() {
  if (selected > 0) {
    selected--;
    dirty = true;
  }
}

void FontMenu::handleButtonPress() {
  const char *filename = buttons[selected]->GetLabel();
  if (!filename) {
    app->PrintStatus("error");
    return;
  }

  switch (app->mode) {
  case APP_MODE_PREFS_FONT:
    app->ts->SetFontFile(filename, TEXT_STYLE_REGULAR);
    app->PrefsRefreshButton(PREFS_BUTTON_FONT_CONFIG);
    break;

  case APP_MODE_PREFS_FONT_BOLD:
    app->ts->SetFontFile(filename, TEXT_STYLE_BOLD);
    app->PrefsRefreshButton(PREFS_BUTTON_FONT_CONFIG);
    break;

  case APP_MODE_PREFS_FONT_ITALIC:
    app->ts->SetFontFile(filename, TEXT_STYLE_ITALIC);
    app->PrefsRefreshButton(PREFS_BUTTON_FONT_CONFIG);
    break;

  case APP_MODE_PREFS_FONT_BOLDITALIC:
    app->ts->SetFontFile(filename, TEXT_STYLE_BOLDITALIC);
    app->PrefsRefreshButton(PREFS_BUTTON_FONT_CONFIG);
    break;
  }

  app->prefs->Write();
  app->ShowSettingsView();
}

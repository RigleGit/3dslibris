#include "font.h"

#include <3ds.h>
#include <dirent.h>
#include <string>
#include <vector>

#include "app.h"
#include "button.h"

#define MIN(x, y) (x < y ? x : y)
#define MAX(x, y) (x > y ? x : y)

FontMenu::FontMenu(App *_app) : Menu(_app) {
  dir = app->fontdir;
  findFiles();
  for (auto &filename : files) {
    Button *b = new Button(app->ts);
    b->Init();
    b->Move(0, (buttons.size() % pagesize) * b->GetHeight());
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
  touchPosition coord = app->TouchRead();

  if (app->buttonprefs.EnclosesPoint(coord.px, coord.py)) {
    app->ShowSettingsView();
  } else if (app->buttonnext.EnclosesPoint(coord.px, coord.py)) {
    nextPage();
  } else if (app->buttonprev.EnclosesPoint(coord.px, coord.py)) {
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
  for (u8 i = page * pagesize;
       (i < buttons.size()) && (i < (page + 1) * pagesize); i++) {
    buttons[i]->Draw(i == selected);
  }
  if (page > 0)
    app->buttonprev.Draw();
  app->buttonprefs.Label("cancel");
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
    app->PrefsRefreshButton(PREFS_BUTTON_FONT);
    break;

  case APP_MODE_PREFS_FONT_BOLD:
    app->ts->SetFontFile(filename, TEXT_STYLE_BOLD);
    app->PrefsRefreshButton(PREFS_BUTTON_FONT_BOLD);
    break;

  case APP_MODE_PREFS_FONT_ITALIC:
    app->ts->SetFontFile(filename, TEXT_STYLE_ITALIC);
    app->PrefsRefreshButton(PREFS_BUTTON_FONT_ITALIC);
    break;

  case APP_MODE_PREFS_FONT_BOLDITALIC:
    app->ts->SetFontFile(filename, TEXT_STYLE_BOLDITALIC);
    app->PrefsRefreshButton(PREFS_BUTTON_FONT_BOLDITALIC);
    break;
  }

  app->prefs->Write();
  app->ShowSettingsView();
}

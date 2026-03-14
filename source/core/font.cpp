/*
    3dslibris - font.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Font selector/menu flow (target/style/file views).
    - Touch + physical input handling for paged font lists.
    - Runtime font application and UI refresh coordination.
*/

#include "font.h"

#include <3ds.h>
#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <string>
#include <vector>

#include "app.h"
#include "button.h"
#include "main.h"
#include "string_utils.h"
#include "text.h"
#include "touch_utils.h"

#define MIN(x, y) (x < y ? x : y)

namespace {

enum FontTarget : u8 {
  FONT_TARGET_REGULAR = 0,
  FONT_TARGET_BOLD = 1,
  FONT_TARGET_ITALIC = 2,
  FONT_TARGET_BOLDITALIC = 3,
  FONT_TARGET_BROWSER = 4,
  FONT_TARGET_COUNT = 5
};

static const char *kFontTargetLabels[FONT_TARGET_COUNT] = {
    "regular font", "bold font", "italic font", "bold italic font",
    "mono/ui font"};

static std::string BasenameOnly(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos)
    return path;
  return path.substr(slash + 1);
}

static bool FontNameEquals(const std::string &a, const std::string &b) {
  return ToLowerAscii(BasenameOnly(a)) == ToLowerAscii(BasenameOnly(b));
}

static u8 StyleFromTarget(u8 target) {
  switch (target) {
  case FONT_TARGET_BOLD:
    return TEXT_STYLE_BOLD;
  case FONT_TARGET_ITALIC:
    return TEXT_STYLE_ITALIC;
  case FONT_TARGET_BOLDITALIC:
    return TEXT_STYLE_BOLDITALIC;
  case FONT_TARGET_BROWSER:
    return TEXT_STYLE_BROWSER;
  case FONT_TARGET_REGULAR:
  default:
    return TEXT_STYLE_REGULAR;
  }
}

static const char *DefaultFontForStyle(u8 style) {
  switch (style) {
  case TEXT_STYLE_BOLD:
    return FONTBOLDFILE;
  case TEXT_STYLE_ITALIC:
    return FONTITALICFILE;
  case TEXT_STYLE_BOLDITALIC:
    return FONTBOLDITALICFILE;
  case TEXT_STYLE_BROWSER:
    return FONTBROWSERFILE;
  case TEXT_STYLE_REGULAR:
  default:
    return FONTREGULARFILE;
  }
}

static u8 TargetFromMode(u8 mode) {
  switch (mode) {
  case APP_MODE_PREFS_FONT_BOLD:
    return FONT_TARGET_BOLD;
  case APP_MODE_PREFS_FONT_ITALIC:
    return FONT_TARGET_ITALIC;
  case APP_MODE_PREFS_FONT_BOLDITALIC:
    return FONT_TARGET_BOLDITALIC;
  case APP_MODE_PREFS_FONT:
  default:
    return FONT_TARGET_REGULAR;
  }
}

static void LayoutFileFooterButtons(App *app) {
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

static void LayoutTargetFooterButtons(App *app) {
  app->buttonprefs.Move(86, 292);
  app->buttonprefs.Resize(68, 22);
  app->buttonprefs.Label("back");
}

} // namespace

FontMenu::FontMenu(App *_app)
    : Menu(_app), viewState(VIEW_TARGETS), targetSelected(FONT_TARGET_REGULAR) {
  dir = app->fontdir;
  findFiles();

  std::sort(files.begin(), files.end(),
            [](const std::string &a, const std::string &b) {
              return ToLowerAscii(a) < ToLowerAscii(b);
            });

  for (auto &filename : files) {
    Button *b = new Button(app->ts);
    b->Init();
    b->Move(5, 24 + (buttons.size() % pagesize) * 36);
    b->Resize(230, 34);
    b->SetLabel1(std::string(filename));
    buttons.push_back(b);
  }

  for (u8 i = 0; i < FONT_TARGET_COUNT; i++) {
    Button *b = new Button(app->ts);
    b->Init();
    b->SetStyle(BUTTON_STYLE_SETTING);
    b->Move(5, 24 + i * 38);
    b->Resize(230, 36);
    b->SetLabel1(std::string(kFontTargetLabels[i]));
    targetButtons.push_back(b);
  }

  refreshTargetButtons();
}

FontMenu::~FontMenu() {
  for (auto button : buttons) {
    delete button;
  }
  buttons.clear();

  for (auto button : targetButtons) {
    delete button;
  }
  targetButtons.clear();
}

void FontMenu::Open(u8 requested_mode) { enterTargetView(requested_mode); }

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

void FontMenu::refreshTargetButtons() {
  for (u8 i = 0; i < targetButtons.size() && i < FONT_TARGET_COUNT; i++) {
    u8 style = StyleFromTarget(i);
    std::string current = app->ts->GetFontFile(style);
    if (current.empty())
      current = DefaultFontForStyle(style);
    targetButtons[i]->SetLabel2(BasenameOnly(current));
  }
}

void FontMenu::enterTargetView(u8 requested_mode) {
  viewState = VIEW_TARGETS;
  if (requested_mode < FONT_TARGET_COUNT)
    targetSelected = requested_mode;
  else
    targetSelected = TargetFromMode(requested_mode);
  refreshTargetButtons();
  dirty = true;
}

void FontMenu::enterFileView() {
  viewState = VIEW_FILES;
  selected = 0;
  page = 0;

  if (buttons.empty()) {
    dirty = true;
    return;
  }

  u8 style = StyleFromTarget(targetSelected);
  std::string current = app->ts->GetFontFile(style);
  if (current.empty())
    current = DefaultFontForStyle(style);

  for (size_t i = 0; i < files.size(); i++) {
    if (FontNameEquals(files[i], current)) {
      selected = (u8)i;
      break;
    }
  }

  page = selected / pagesize;
  dirty = true;
}

void FontMenu::handleInput() {
  u32 keys = hidKeysDown();
  if (viewState == VIEW_TARGETS)
    handleTargetInput(keys);
  else
    handleFileInput(keys);
}

void FontMenu::handleTargetInput(u32 keys) {
  auto key = app->key;
  if (keys & KEY_B) {
    app->ShowSettingsView(app->IsBookSettingsContext());
  } else if (keys & KEY_A) {
    enterFileView();
  } else if (keys & (key.down | KEY_DOWN | key.right | KEY_RIGHT |
                     KEY_CPAD_DOWN | KEY_CPAD_RIGHT)) {
    selectNextTarget();
  } else if (keys & (key.up | KEY_UP | key.left | KEY_LEFT | KEY_CPAD_UP |
                     KEY_CPAD_LEFT)) {
    selectPreviousTarget();
  } else if (keys & KEY_TOUCH) {
    handleTargetTouchInput();
  }
}

void FontMenu::handleFileInput(u32 keys) {
  auto key = app->key;
  if (keys & KEY_B) {
    enterTargetView(targetSelected);
  } else if (keys & KEY_A) {
    handleButtonPress();
  } else if (keys & (key.down | KEY_DOWN | key.right | KEY_RIGHT |
                     KEY_CPAD_DOWN | KEY_CPAD_RIGHT)) {
    selectNext();
  } else if (keys & (key.up | KEY_UP | key.left | KEY_LEFT | KEY_CPAD_UP |
                     KEY_CPAD_LEFT)) {
    selectPrevious();
  } else if (keys & (key.r | KEY_R)) {
    nextPage();
  } else if (keys & (key.l | KEY_L)) {
    previousPage();
  } else if (keys & KEY_TOUCH) {
    handleFileTouchInput();
  }
}

void FontMenu::handleTargetTouchInput() {
  LayoutTargetFooterButtons(app);
  TouchCandidates candidates;
  touch::BuildCandidates(app, &candidates);

  if (touch::HitsButton(candidates, &app->buttonprefs, 4)) {
    app->ShowSettingsView(app->IsBookSettingsContext());
    return;
  }

  for (u8 i = 0; i < targetButtons.size() && i < FONT_TARGET_COUNT; i++) {
    if (touch::HitsButton(candidates, targetButtons[i], 4)) {
      targetSelected = i;
      enterFileView();
      return;
    }
  }

  int footerX = -1;
  if (touch::FirstXInBottomBand(candidates, 284, &footerX)) {
    app->ShowSettingsView(app->IsBookSettingsContext());
    return;
  }
}

void FontMenu::handleFileTouchInput() {
  LayoutFileFooterButtons(app);
  TouchCandidates candidates;
  touch::BuildCandidates(app, &candidates);

  int footerX = -1;
  touch::FirstXInBottomBand(candidates, 284, &footerX);

  for (u8 i = page * pagesize;
       (i < buttons.size()) && (i < (page + 1) * pagesize); i++) {
    if (touch::HitsButton(candidates, buttons[i], 4)) {
      selected = i;
      handleButtonPress();
      return;
    }
  }

  if (touch::HitsButton(candidates, &app->buttonprefs, 4)) {
    enterTargetView(targetSelected);
    return;
  }

  if (page < GetPageCount() - 1 &&
      touch::HitsButton(candidates, &app->buttonnext, 4)) {
    nextPage();
    return;
  }

  if (page > 0 && touch::HitsButton(candidates, &app->buttonprev, 4)) {
    previousPage();
    return;
  }

  if (footerX < 0)
    return;

  if (footerX < 80) {
    if (page > 0)
      previousPage();
    else
      enterTargetView(targetSelected);
    return;
  }

  if (footerX < 160) {
    enterTargetView(targetSelected);
    return;
  }

  if (page < GetPageCount() - 1) {
    nextPage();
  } else {
    // Last page has no "next": treat right-bottom band as back.
    enterTargetView(targetSelected);
  }
}

void FontMenu::draw() {
  app->ts->ClearScreen();
  app->DrawBottomGradientBackground();

  if (viewState == VIEW_TARGETS) {
    LayoutTargetFooterButtons(app);
    app->ts->SetPen(6, 14);
    app->ts->PrintString("font configuration");

    for (u8 i = 0; i < targetButtons.size() && i < FONT_TARGET_COUNT; i++) {
      targetButtons[i]->Draw(i == targetSelected);
    }

    app->buttonprefs.Draw();
    dirty = false;
    return;
  }

  LayoutFileFooterButtons(app);
  app->ts->SetPen(6, 14);
  char header[72];
  snprintf(header, sizeof(header), "select %s",
           kFontTargetLabels[targetSelected]);
  app->ts->PrintString(header);

  for (u8 i = page * pagesize;
       (i < buttons.size()) && (i < (page + 1) * pagesize); i++) {
    buttons[i]->Draw(i == selected);
  }

  char pageLabel[24];
  snprintf(pageLabel, sizeof(pageLabel), "Pg %d/%d", GetCurrentPage(),
           GetPageCount());
  app->ts->SetPen(6, 282);
  app->ts->PrintString(pageLabel);

  if (page > 0)
    app->buttonprev.Draw();
  app->buttonprefs.Draw();
  if (page < GetPageCount() - 1)
    app->buttonnext.Draw();
  dirty = false;
}

void FontMenu::selectNext() {
  if (buttons.empty())
    return;
  if (selected < buttons.size() - 1) {
    selected++;
    page = selected / pagesize;
    dirty = true;
  }
}

void FontMenu::nextPage() {
  if (buttons.empty())
    return;
  if ((u16)(page + 1) * pagesize < buttons.size()) {
    page += 1;
    selected = page * pagesize;
    dirty = true;
  }
}

void FontMenu::previousPage() {
  if (buttons.empty())
    return;
  if (page > 0) {
    page--;
    selected = page * pagesize;
    dirty = true;
  }
}

void FontMenu::selectPrevious() {
  if (buttons.empty())
    return;
  if (selected > 0) {
    selected--;
    page = selected / pagesize;
    dirty = true;
  }
}

void FontMenu::selectNextTarget() {
  if (targetSelected + 1 < FONT_TARGET_COUNT) {
    targetSelected++;
    dirty = true;
  }
}

void FontMenu::selectPreviousTarget() {
  if (targetSelected > 0) {
    targetSelected--;
    dirty = true;
  }
}

void FontMenu::handleButtonPress() {
  if (buttons.empty())
    return;

  const char *filename = buttons[selected]->GetLabel();
  if (!filename || !*filename) {
    app->PrintStatus("error");
    return;
  }

  const u8 style = StyleFromTarget(targetSelected);
  const std::string previous = app->ts->GetFontFile(style);
  app->ts->SetFontFile(filename, style);
  if (style != TEXT_STYLE_BROWSER && previous != filename)
    app->MarkBookLayoutDirty();
  app->PrefsRefreshButton(PREFS_BUTTON_FONT_CONFIG);
  app->prefs->Write();
  refreshTargetButtons();
  enterTargetView(targetSelected);
}

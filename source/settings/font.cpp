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

#include "settings/font.h"

#include <3ds.h>
#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <string>
#include <vector>

#include "app/app_mode.h"
#include "shared/debug_log.h"
#include "ui/button.h"
#include "ui/font_constants.h"
#include "ui/gradient_utils.h"
#include "settings/font_config_utils.h"
#include "settings/prefs_button_ids.h"
#include "shared/bugfix_utils.h"
#include "shared/orientation_utils.h"
#include "shared/string_utils.h"
#include "shared/text_unicode_utils.h"
#include "shared/text_wrap_utils.h"
#include "ui/button_text_layout.h"
#include "ui/text.h"
#include "ui/touch_utils.h"

namespace {

static const int kRowX = 5;
static const int kRowTop = 24;
// Rows end above the footer buttons; the page indicator lives in the header.
static const int kRowsBottom = 288;
static const int kRowW = 230;
static const int kRowMinH = 36;
static const int kRowGap = 2;
static const int kHeaderY = 14;

// Wraps every label of `b` inside a row of kRowW and returns the box height
// that holds all lines, so no name is ever cut or clipped.
static int WrappedRowHeight(Text *ts, Button *b) {
  const int saved_style = ts->GetStyle();
  ts->SetStyle(TEXT_STYLE_BROWSER);
  const int line_h = ts->GetHeight();
  const int ascent = ts->GetAscent();
  const int descent = ts->GetDescent();
  ts->SetStyle(saved_style);
  b->SetWrap(true);
  const int lines = b->WrappedLineCount(kRowW);
  int h = button_text_layout::HeightForLines(lines, line_h, ascent, descent,
                                             kRowMinH);
  if (h > kRowsBottom - kRowTop)
    h = kRowsBottom - kRowTop; // a single huge row still gets a page
  return h;
}

// Sizes every button, paginates them by cumulative height and places each one
// inside its page. `sizes` receives the number of rows of every page.
static void LayoutButtonPages(Text *ts, std::vector<Button *> &rows,
                              std::vector<int> *sizes) {
  std::vector<int> heights;
  heights.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); i++) {
    const int h = WrappedRowHeight(ts, rows[i]);
    rows[i]->Resize(kRowW, (u16)h);
    heights.push_back(h);
  }
  std::vector<int> ys;
  text_wrap_utils::LayoutRows(heights.data(), (int)heights.size(), kRowTop,
                              kRowsBottom, kRowGap, sizes, &ys);
  for (size_t i = 0; i < rows.size(); i++)
    rows[i]->Move(kRowX, (u16)ys[i]);
}

static int PageStartOf(const std::vector<int> &sizes, int page) {
  int start = 0;
  for (int i = 0; i < page && i < (int)sizes.size(); i++)
    start += sizes[i];
  return start;
}

static int PageOfIndex(const std::vector<int> &sizes, int index) {
  int start = 0;
  for (size_t i = 0; i < sizes.size(); i++) {
    if (index < start + sizes[i])
      return (int)i;
    start += sizes[i];
  }
  return sizes.empty() ? 0 : (int)sizes.size() - 1;
}

// "Pg n/m" sits in the header, right-aligned, so it never shares space with
// the rows.
static void DrawPageIndicator(Text *ts, int current, int total) {
  char label[24];
  snprintf(label, sizeof(label), "Pg %d/%d", current, total);
  const bool saved_wrap = ts->IsAutoWrapEnabled();
  const int saved_margin_right = ts->margin.right;
  ts->SetAutoWrapEnabled(false);
  ts->margin.right = 0;
  ts->SetPen(std::max(6, kRowX + kRowW - ts->GetStringAdvance(label)), kHeaderY);
  ts->PrintString(label);
  ts->margin.right = saved_margin_right;
  ts->SetAutoWrapEnabled(saved_wrap);
}

static std::string BasenameOnly(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos)
    return path;
  return path.substr(slash + 1);
}

static bool FontNameEquals(const std::string &a, const std::string &b) {
  return ToLowerAscii(BasenameOnly(a)) == ToLowerAscii(BasenameOnly(b));
}

static u8 TargetFromMode(AppMode mode) {
  switch (mode) {
  case AppMode::PrefsFontBold:
    return font_config_utils::FONT_TARGET_BOLD;
  case AppMode::PrefsFontItalic:
    return font_config_utils::FONT_TARGET_ITALIC;
  case AppMode::PrefsFontBoldItalic:
    return font_config_utils::FONT_TARGET_BOLDITALIC;
  case AppMode::PrefsFont:
  default:
    return font_config_utils::FONT_TARGET_REGULAR;
  }
}

static void LayoutFileFooterButtons(Button *buttonprev, Button *buttonnext,
                                     Button *buttonprefs) {
  buttonprev->Move(6, 292);
  buttonprev->Resize(68, 22);
  buttonprev->Label("prev");

  buttonprefs->Move(86, 292);
  buttonprefs->Resize(68, 22);
  buttonprefs->Label("back");

  buttonnext->Move(166, 292);
  buttonnext->Resize(68, 22);
  buttonnext->Label("next");
}

static void LayoutTargetFooterButtons(Button *buttonprefs) {
  buttonprefs->Move(86, 292);
  buttonprefs->Resize(68, 22);
  buttonprefs->Label("back");
}

} // namespace

FontMenu::FontMenu(const FontMenuContext &context)
    : Menu(context.base), context_(context), viewState(VIEW_TARGETS),
      targetSelected(font_config_utils::FONT_TARGET_REGULAR),
      targetPage(0) {
  dir = context_.FontDir();
  findFiles();

  std::sort(files.begin(), files.end(),
            [](const std::string &a, const std::string &b) {
              return ToLowerAscii(a) < ToLowerAscii(b);
            });

  for (auto &filename : files) {
    Button *b = new Button(ts);
    b->Init();
    b->Move(kRowX, kRowTop);
    b->SetLabel1(filename);
    b->Resize(kRowW, kRowMinH);
    buttons.push_back(b);
  }

  for (u8 i = 0; i < font_config_utils::FONT_TARGET_COUNT; i++) {
    Button *b = new Button(ts);
    b->Init();
    b->SetStyle(BUTTON_STYLE_SETTING);
    b->Move(kRowX, kRowTop);
    b->Resize(kRowW, kRowMinH);
    b->SetLabel1(std::string(font_config_utils::GetFontTargetLabel(i)));
    targetButtons.push_back(b);
  }

  refreshTargetButtons();
  relayoutTargets();
  relayoutFiles();
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

void FontMenu::Open(AppMode requested_mode) {
  enterTargetView(TargetFromMode(requested_mode));
}

void FontMenu::findFiles() {
  std::vector<std::string> search_dirs;
  GetFallbackFontSearchDirs(dir, paths::GetFontDir().c_str(), &search_dirs);

  std::vector<std::string> seen;
  for (size_t d = 0; d < search_dirs.size(); d++) {
    DIR *dp = opendir(search_dirs[d].c_str());
    if (!dp)
      continue;

    struct dirent *ent;
    while ((ent = readdir(dp))) {
      if (ent->d_type == DT_DIR)
        continue;

      const char *filename = ent->d_name;
      const char *c = strrchr(filename, '.');
      if (!c)
        continue;
      if (strcmp(".ttf", c) && strcmp(".otf", c) && strcmp(".ttc", c))
        continue;

      const std::string lower = ToLowerAscii(std::string(filename));
      bool duplicate = false;
      for (size_t i = 0; i < seen.size(); i++) {
        if (seen[i] == lower) {
          duplicate = true;
          break;
        }
      }
      if (duplicate)
        continue;

      seen.push_back(lower);
      files.push_back(std::string(filename));
    }
    closedir(dp);
  }
}

void FontMenu::refreshTargetButtons() {
  for (u8 i = 0; i < targetButtons.size() &&
                  i < font_config_utils::FONT_TARGET_COUNT;
       i++) {
    if (font_config_utils::IsFallbackTarget(i)) {
      int fb_idx = font_config_utils::FallbackIndexFromTarget(i);
      std::string fb_file = ts->fm->GetFallbackFile(fb_idx);
      targetButtons[i]->SetLabel2(fb_file.empty() ? "(none)" : BasenameOnly(fb_file));
    } else {
      u8 style = font_config_utils::StyleFromTarget(i);
      std::string current = ts->GetFontFile(style);
      if (current.empty())
        current = font_config_utils::DefaultFontForStyle(style);
      targetButtons[i]->SetLabel2(BasenameOnly(current));
    }
  }
  relayoutTargets();
}

void FontMenu::relayoutTargets() {
  LayoutButtonPages(ts, targetButtons, &targetPageSizes);
  if (targetPage >= targetPageSizes.size())
    targetPage = targetPageSizes.empty() ? 0 : (u8)(targetPageSizes.size() - 1);
}

void FontMenu::relayoutFiles() {
  LayoutButtonPages(ts, buttons, &filePageSizes);
  if (page >= filePageSizes.size())
    page = filePageSizes.empty() ? 0 : (u16)(filePageSizes.size() - 1);
}

void FontMenu::enterTargetView(u8 requested_target) {
  viewState = VIEW_TARGETS;
  if (requested_target < font_config_utils::FONT_TARGET_COUNT)
    targetSelected = requested_target;
  syncTargetPageToSelection();
  refreshTargetButtons();
  dirty = true;
}

void FontMenu::enterFileView() {
  viewState = VIEW_FILES;
  selected = 0;
  page = 0;
  relayoutFiles();

  if (buttons.empty()) {
    dirty = true;
    return;
  }

  u8 style = font_config_utils::StyleFromTarget(targetSelected);
  std::string current = ts->GetFontFile(style);
  if (current.empty())
    current = font_config_utils::DefaultFontForStyle(style);

  for (size_t i = 0; i < files.size(); i++) {
    if (FontNameEquals(files[i], current)) {
      selected = (u16)i;
      break;
    }
  }

  relayoutFiles();
  page = (u16)PageOfIndex(filePageSizes, selected);
  dirty = true;
}

void FontMenu::handleInput(const FrameInput &input) {
#ifdef DSLIBRIS_DEBUG
  const u32 keys = input.keys_down;
  static int s_font_input_budget = 96;
  if (status_reporter && s_font_input_budget > 0 &&
      (keys != 0 || input.keys_held != 0)) {
    DBG_LOGF(status_reporter,
             "FONT input keys=0x%08lx held=0x%08lx view=%d target=%u sel=%u page=%u dirty=%d",
             (unsigned long)keys, (unsigned long)input.keys_held,
             (int)viewState, (unsigned)targetSelected, (unsigned)selected,
             (unsigned)page, dirty ? 1 : 0);
    s_font_input_budget--;
  }
#endif
  if (viewState == VIEW_TARGETS)
    handleTargetInput(input);
  else
    handleFileInput(input);
}

void FontMenu::handleTargetInput(const FrameInput &input) {
  const u32 keys = input.keys_down;
  const KeyMap &key = context_.keys;
  const bool turned_right = orientation_utils::IsTurnedRight(ts->GetOrientation());
  const u32 list_next_keys = turned_right
      ? key.dup | key.dright | key.up | key.right
      : key.ddown | key.dleft | key.down | key.left;
  const u32 list_prev_keys = turned_right
      ? key.ddown | key.dleft | key.down | key.left
      : key.dup | key.dright | key.up | key.right;
  const u32 next_page_key = turned_right ? key.l : key.r;
  const u32 previous_page_key = turned_right ? key.r : key.l;
  if (keys & key.b) {
    context_.ShowSettingsView(context_.IsBookSettingsContext());
  } else if (keys & key.a) {
    enterFileView();
  } else if (keys & next_page_key) {
    nextTargetPage();
  } else if (keys & previous_page_key) {
    previousTargetPage();
  } else if (keys & list_next_keys) {
    selectNextTarget();
  } else if (keys & list_prev_keys) {
    selectPreviousTarget();
  } else if (keys & KEY_TOUCH) {
    handleTargetTouchInput(input);
  }
}

void FontMenu::handleFileInput(const FrameInput &input) {
  const u32 keys = input.keys_down;
  const KeyMap &key = context_.keys;
  const bool turned_right = orientation_utils::IsTurnedRight(ts->GetOrientation());
  const u32 list_next_keys = turned_right
      ? key.dup | key.dright | key.up | key.right
      : key.ddown | key.dleft | key.down | key.left;
  const u32 list_prev_keys = turned_right
      ? key.ddown | key.dleft | key.down | key.left
      : key.dup | key.dright | key.up | key.right;
  const u32 next_page_key = turned_right ? key.l : key.r;
  const u32 previous_page_key = turned_right ? key.r : key.l;
  if (keys & key.b) {
    enterTargetView(targetSelected);
  } else if (keys & key.a) {
    handleButtonPress();
  } else if (keys & list_next_keys) {
    selectNext();
  } else if (keys & list_prev_keys) {
    selectPrevious();
  } else if (keys & next_page_key) {
    nextPage();
  } else if (keys & previous_page_key) {
    previousPage();
  } else if (keys & KEY_TOUCH) {
    handleFileTouchInput(input);
  }
}

void FontMenu::handleTargetTouchInput(const FrameInput &input) {
  LayoutTargetFooterButtons(buttonprefs);
  TouchCandidates candidates;
  touch::BuildCandidates(context_.MapTouch(input), &candidates);

  if (targetPage + 1 < getTargetPageCount() &&
      touch::HitsButton(candidates, buttonnext, 4)) {
    nextTargetPage();
    return;
  }

  if (targetPage > 0 && touch::HitsButton(candidates, buttonprev, 4)) {
    previousTargetPage();
    return;
  }

  if (touch::HitsButton(candidates, buttonprefs, 4)) {
    context_.ShowSettingsView(context_.IsBookSettingsContext());
    return;
  }

  const u8 start = getTargetPageStart();
  const u8 end = getTargetPageEnd();
  for (u8 i = start; i < end; i++) {
    if (touch::HitsButton(candidates, targetButtons[i], 4)) {
      targetSelected = i;
      enterFileView();
      return;
    }
  }

  int footerX = -1;
  if (touch::FirstXInBottomBand(candidates, 284, &footerX)) {
    if (footerX < 80) {
      if (targetPage > 0)
        previousTargetPage();
      else
        context_.ShowSettingsView(context_.IsBookSettingsContext());
      return;
    }
    if (footerX > 160) {
      if (targetPage + 1 < getTargetPageCount())
        nextTargetPage();
      else
        context_.ShowSettingsView(context_.IsBookSettingsContext());
      return;
    }
    context_.ShowSettingsView(context_.IsBookSettingsContext());
    return;
  }
}

void FontMenu::handleFileTouchInput(const FrameInput &input) {
  LayoutFileFooterButtons(buttonprev, buttonnext, buttonprefs);
  TouchCandidates candidates;
  touch::BuildCandidates(context_.MapTouch(input), &candidates);

  int footerX = -1;
  touch::FirstXInBottomBand(candidates, 284, &footerX);

  const u16 file_start = (u16)PageStartOf(filePageSizes, page);
  const u16 file_end = (u16)std::min((size_t)(file_start + (page < filePageSizes.size() ? filePageSizes[page] : 0)), buttons.size());
  for (u16 i = file_start; i < file_end; i++) {
    if (touch::HitsButton(candidates, buttons[i], 4)) {
      selected = i;
      handleButtonPress();
      return;
    }
  }

  if (touch::HitsButton(candidates, buttonprefs, 4)) {
    enterTargetView(targetSelected);
    return;
  }

  if (page < GetPageCount() - 1 &&
      touch::HitsButton(candidates, buttonnext, 4)) {
    nextPage();
    return;
  }

  if (page > 0 && touch::HitsButton(candidates, buttonprev, 4)) {
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
#ifdef DSLIBRIS_DEBUG
  static int s_font_draw_budget = 24;
  if (s_font_draw_budget > 0) {
    const u16 before0 = ts->screenright[0];
    const u16 before1 = ts->screenright[(size_t)10 * (size_t)ts->display.height + 10];
    DBG_LOGF(status_reporter,
             "FONT draw begin view=%d target=%u sel=%u page=%u dirty=%d app=%p before0=%04x before1=%04x",
             (int)viewState, (unsigned)targetSelected, (unsigned)selected,
             (unsigned)page, dirty ? 1 : 0, (void *)app, (unsigned)before0,
             (unsigned)before1);
    s_font_draw_budget--;
  }
#endif
  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  gradient_utils::DrawToScreen(ts, color_mode ? (int)*color_mode : 0,
                               ts->screenright, 320);

  if (viewState == VIEW_TARGETS) {
    LayoutTargetFooterButtons(buttonprefs);
    ts->SetPen(6, kHeaderY);
    ts->PrintString("font configuration");

    const u8 start = getTargetPageStart();
    const u8 end = getTargetPageEnd();
    for (u8 i = start; i < end; i++) {
      targetButtons[i]->Draw(i == targetSelected);
    }

    DrawPageIndicator(ts, getTargetCurrentPage(), getTargetPageCount());

    if (targetPage > 0)
      buttonprev->Draw();
    buttonprefs->Draw();
    if (targetPage + 1 < getTargetPageCount())
      buttonnext->Draw();
    dirty = false;
#ifdef DSLIBRIS_DEBUG
    static int s_font_draw_end_budget = 24;
    if (s_font_draw_end_budget > 0) {
      const u16 after0 = ts->screenright[0];
      const u16 after1 =
          ts->screenright[(size_t)10 * (size_t)ts->display.height + 10];
      DBG_LOGF(status_reporter,
               "FONT draw end view=targets dirty=%d after0=%04x after1=%04x",
               dirty ? 1 : 0, (unsigned)after0, (unsigned)after1);
      s_font_draw_end_budget--;
    }
#endif
    return;
  }

  LayoutFileFooterButtons(buttonprev, buttonnext, buttonprefs);
  ts->SetPen(6, kHeaderY);
  char header[72];
  snprintf(header, sizeof(header), "select %s",
           font_config_utils::GetFontTargetLabel(targetSelected));
  ts->PrintString(header);

  const u16 draw_start = (u16)PageStartOf(filePageSizes, page);
  const u16 draw_end = (u16)std::min(
      (size_t)(draw_start + (page < filePageSizes.size() ? filePageSizes[page] : 0)),
      buttons.size());
  for (u16 i = draw_start; i < draw_end; i++)
    buttons[i]->Draw(i == selected);

  DrawPageIndicator(ts, GetCurrentPage(), GetPageCount());

  if (page > 0)
    buttonprev->Draw();
  buttonprefs->Draw();
  if (page < GetPageCount() - 1)
    buttonnext->Draw();
  dirty = false;
#ifdef DSLIBRIS_DEBUG
  static int s_font_draw_end_budget2 = 24;
  if (s_font_draw_end_budget2 > 0) {
    const u16 after0 = ts->screenright[0];
    const u16 after1 =
        ts->screenright[(size_t)10 * (size_t)ts->display.height + 10];
    DBG_LOGF(status_reporter, "FONT draw end view=files dirty=%d after0=%04x after1=%04x",
             dirty ? 1 : 0, (unsigned)after0, (unsigned)after1);
    s_font_draw_end_budget2--;
  }
#endif
}

u16 FontMenu::GetPageCount() const {
  return filePageSizes.empty() ? 1 : (u16)filePageSizes.size();
}

u16 FontMenu::GetCurrentPage() const { return page + 1; }

void FontMenu::selectNext() {
  if (buttons.empty())
    return;
  if (selected < buttons.size() - 1) {
    selected++;
    page = (u16)PageOfIndex(filePageSizes, selected);
    dirty = true;
  }
}

void FontMenu::nextPage() {
  if (buttons.empty())
    return;
  if (page + 1 < GetPageCount()) {
    page += 1;
    selected = (u16)PageStartOf(filePageSizes, page);
    dirty = true;
  }
}

void FontMenu::previousPage() {
  if (buttons.empty())
    return;
  if (page > 0) {
    page--;
    selected = (u16)PageStartOf(filePageSizes, page);
    dirty = true;
  }
}

void FontMenu::selectPrevious() {
  if (buttons.empty())
    return;
  if (selected > 0) {
    selected--;
    page = (u16)PageOfIndex(filePageSizes, selected);
    dirty = true;
  }
}

void FontMenu::selectNextTarget() {
  if (targetSelected + 1 < font_config_utils::FONT_TARGET_COUNT) {
    targetSelected++;
    syncTargetPageToSelection();
    dirty = true;
  }
}

void FontMenu::selectPreviousTarget() {
  if (targetSelected > 0) {
    targetSelected--;
    syncTargetPageToSelection();
    dirty = true;
  }
}

u8 FontMenu::getTargetPageCount() const {
  return targetPageSizes.empty() ? 1 : (u8)targetPageSizes.size();
}

u8 FontMenu::getTargetCurrentPage() const { return targetPage + 1; }

u8 FontMenu::getTargetPageStart() const {
  return (u8)PageStartOf(targetPageSizes, targetPage);
}

u8 FontMenu::getTargetPageEnd() const {
  const int size = targetPage < targetPageSizes.size() ? targetPageSizes[targetPage] : 0;
  const int end = getTargetPageStart() + size;
  return (u8)(end < font_config_utils::FONT_TARGET_COUNT
                  ? end
                  : font_config_utils::FONT_TARGET_COUNT);
}

void FontMenu::syncTargetPageToSelection() {
  targetPage = (u8)PageOfIndex(targetPageSizes, targetSelected);
}

void FontMenu::nextTargetPage() {
  const u8 page_count = getTargetPageCount();
  if (targetPage + 1 >= page_count)
    return;
  targetPage++;
  const u8 start = getTargetPageStart();
  const u8 end = getTargetPageEnd();
  if (targetSelected < start || targetSelected >= end)
    targetSelected = start;
  dirty = true;
}

void FontMenu::previousTargetPage() {
  if (targetPage == 0)
    return;
  targetPage--;
  const u8 start = getTargetPageStart();
  const u8 end = getTargetPageEnd();
  if (targetSelected < start || targetSelected >= end)
    targetSelected = end - 1;
  dirty = true;
}

void FontMenu::handleButtonPress() {
  if (buttons.empty())
    return;

  const char *filename = buttons[selected]->GetLabel();
  if (!filename || !*filename) {
    context_.PrintStatus("error");
    return;
  }

  if (font_config_utils::IsFallbackTarget(targetSelected)) {
    int fb_idx = font_config_utils::FallbackIndexFromTarget(targetSelected);
    if (ts->fm->SetFallbackFile(fb_idx, filename)) {
      char msg[64];
      snprintf(msg, sizeof(msg), "fallback %d: %s", fb_idx + 1, BasenameOnly(filename).c_str());
      context_.PrintStatus(msg);
      context_.MarkBookLayoutDirty();
      const int write_rc = context_.WritePrefs();
#ifdef DSLIBRIS_DEBUG
      DBG_LOGF(status_reporter, "FONT prefs write rc=%d fallback=%d file=%s", write_rc,
               fb_idx + 1, ts->fm->GetFallbackFile(fb_idx).c_str());
#endif
    } else {
      context_.PrintStatus("error loading fallback font");
    }
  } else {
    const u8 style = font_config_utils::StyleFromTarget(targetSelected);
    const std::string previous = ts->GetFontFile(style);
    ts->SetFontFile(filename, style);
    const std::string current = ts->GetFontFile(style);
    if (current != std::string(filename)) {
      context_.PrintStatus("error loading font");
#ifdef DSLIBRIS_DEBUG
      DBG_LOGF(status_reporter,
               "FONT apply failed style=%u requested=%s current=%s previous=%s",
               (unsigned)style, filename, current.c_str(), previous.c_str());
#endif
      refreshTargetButtons();
      enterTargetView(targetSelected);
      return;
    }
    if (style != TEXT_STYLE_BROWSER && previous != filename)
      context_.MarkBookLayoutDirty();
    context_.RefreshPrefsButton(PREFS_BUTTON_FONT_CONFIG);
    const int write_rc = context_.WritePrefs();
#ifdef DSLIBRIS_DEBUG
    DBG_LOGF(status_reporter, "FONT prefs write rc=%d style=%u file=%s", write_rc,
             (unsigned)style, current.c_str());
#endif
  }
  refreshTargetButtons();
  enterTargetView(targetSelected);
}

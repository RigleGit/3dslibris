/*
    3dslibris - button.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Button rendering and hit-testing.
    - UTF-8-safe label truncation for constrained layouts.
    - Integration with procedural button skin and icon draw pipeline.
*/

#include "button.h"

#include <3ds.h>
#include <algorithm>
#include <stdio.h>
#include <string.h>

namespace {

static size_t Utf8BytesForCharCount(Text *ts, const char *s, u8 count) {
  if (!s || !*s || count == 0)
    return 0;

  size_t bytes = 0;
  u8 chars = 0;
  while (s[bytes] && chars < count) {
    u32 ucs = 0;
    u8 step = ts ? ts->GetCharCode(s + bytes, &ucs) : 0;
    if (!step)
      step = 1;
    bytes += step;
    chars++;
  }
  return bytes;
}

} // namespace

Button::Button() {}

Button::Button(Text *t) { Init(t); }

void Button::Init(Text *typesetter) {
  origin.x = 0;
  origin.y = 0;
  extent.x = 192;
  extent.y = 34;
  style = BUTTON_STYLE_BOOK;
  text.pixelsize = 12;
  text.style = TEXT_STYLE_BROWSER;
  text1.clear();
  text2.clear();
  icon = UI_BUTTON_ICON_NONE;
  iconExplicit = false;
  enabled = true;
  ts = typesetter;
  UiButtonSkin_Init();
}

void Button::Label(const char *s) {
  std::string str = s;
  SetLabel(str);
}

void Button::SetLabel1(std::string s) { text1 = s; }

void Button::SetLabel2(std::string s) { text2 = s; }

void Button::Move(u16 x, u16 y) {
  origin.x = x;
  origin.y = y;
}

void Button::Resize(u16 x, u16 y) {
  extent.x = x;
  extent.y = y;
}

UiButtonIconId Button::ResolveIcon() const {
  if (iconExplicit)
    return icon;
  if (extent.x > 140)
    return UI_BUTTON_ICON_NONE;
  if (extent.y < 20 || extent.x < 56)
    return UI_BUTTON_ICON_NONE;
  return UiButtonSkin_IconFromLabel(text1.c_str());
}

void Button::Draw(u16 *screen, bool highlight) {
  if (!ts)
    return;

  auto save_screen = ts->GetScreen();
  auto save_style = ts->GetStyle();

  if (screen == nullptr)
    screen = ts->screen;

  ts->SetScreen(screen);
  ts->SetStyle(text.style);

  const int logicalHeight = (screen == ts->screenleft) ? 400 : 320;
  const int stride = ts->display.height;

  UiButtonIconId resolvedIcon = ResolveIcon();
  bool hasIcon = resolvedIcon != UI_BUTTON_ICON_NONE;
  UiButtonSkinState state = UI_BUTTON_STATE_NORMAL;
  if (!enabled)
    state = UI_BUTTON_STATE_DISABLED;
  else if (highlight)
    state = UI_BUTTON_STATE_SELECTED;

  const int bx = (int)origin.x;
  const int by = (int)origin.y;
  const int bw = (int)extent.x;
  const int bh = (int)extent.y;

  UiButtonSkin_Draw(screen, stride, logicalHeight, bx, by, bw, bh, state,
                    hasIcon);

  u16 old_color_mode = ts->GetColorMode();
  if (old_color_mode != 0)
    ts->SetColorMode(0);

  int line_height = ts->GetHeight();
  int text_x = bx + ((bw <= 90) ? 6 : 8);
  int right_pad = (bw <= 90) ? 6 : 8;
  if (hasIcon)
    right_pad += UiButtonSkin_IconBlockWidth(bh);
  int text_width = bw - (text_x - bx) - right_pad - 2;
  if (text_width < 6)
    text_width = 6;

  if (!text1.empty()) {
    int line_count = text2.empty() ? 1 : 2;
    int block_h = line_count * line_height;
    int top = by + (bh - block_h) / 2;
    if (line_count == 1)
      top -= 2;
    else if (bh <= 22)
      top -= 1;
    if (top < by)
      top = by;
    if (top + block_h > by + bh - 1)
      top = (by + bh - 1) - block_h;

    char line1[256];
    u8 len1 = ts->GetCharCountInsideWidth(text1.c_str(), text.style, text_width);
    size_t bytes1 = Utf8BytesForCharCount(ts, text1.c_str(), len1);
    if (bytes1 > sizeof(line1) - 1)
      bytes1 = sizeof(line1) - 1;
    memcpy(line1, text1.c_str(), bytes1);
    line1[bytes1] = '\0';

    ts->SetPen(text_x, top + line_height);
    ts->PrintString(line1, text.style);

    if (!text2.empty()) {
      char line2[256];
      u8 len2 =
          ts->GetCharCountInsideWidth(text2.c_str(), text.style, text_width);
      size_t bytes2 = Utf8BytesForCharCount(ts, text2.c_str(), len2);
      if (bytes2 > sizeof(line2) - 1)
        bytes2 = sizeof(line2) - 1;
      memcpy(line2, text2.c_str(), bytes2);
      line2[bytes2] = '\0';

      ts->SetPen(text_x, top + line_height * 2);
      ts->PrintString(line2, text.style);
    }
  }

  if (hasIcon) {
    UiButtonSkin_DrawIcon(screen, stride, logicalHeight, bx, by, bw, bh,
                          resolvedIcon, !enabled);
  }

  if (old_color_mode != 0)
    ts->SetColorMode(old_color_mode);
  ts->SetScreen(save_screen);
  ts->SetStyle(save_style);
}

bool Button::EnclosesPoint(u16 x, u16 y) {
  if (x > origin.x && y > origin.y && x < origin.x + extent.x &&
      y < origin.y + extent.y)
    return true;
  return false;
}

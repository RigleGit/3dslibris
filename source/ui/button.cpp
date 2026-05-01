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

#include "ui/button.h"

#include <3ds.h>
#include <algorithm>
#include <stdio.h>
#include <string.h>

#include "shared/text_layout_utils.h"
#include "shared/text_unicode_utils.h"

namespace {

static size_t Utf8BytesForCharCount(Text *ts, const char *s, u8 count) {
  (void)ts;
  if (!s || !*s || count == 0)
    return 0;
  return text_unicode_utils::Utf8BytesForDisplayChars(s, count);
}

static int MeasureStringWidth(Text *ts, const char *s) {
  int w = 0;
  while (*s) {
    u32 cp = 0;
    u8 nb = ts->GetCharCode(s, &cp);
    if (!nb) break;
    w += ts->GetAdvance((u16)cp);
    s += nb;
  }
  return w;
}

static int RtlPenX(Text *ts, const char *line, int text_x, int bx, int bw,
                   int right_pad) {
  int rx = bx + bw - right_pad - 2 - MeasureStringWidth(ts, line);
  return rx < text_x ? text_x : rx;
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
  text3.clear();
  display1.clear();
  display2.clear();
  display3.clear();
  text1_rtl = false;
  text2_rtl = false;
  text3_rtl = false;
  icon = UI_BUTTON_ICON_NONE;
  iconExplicit = false;
  enabled = true;
  ts = typesetter;
  UiButtonSkin_Init();
}

static void PrepareButtonLabel(const std::string &raw, std::string *display,
                               bool *is_rtl) {
  if (raw.empty()) {
    display->clear();
    *is_rtl = false;
    return;
  }
  bool rtl = false;
  text_layout_utils::PrepareDisplayUtf8(raw.c_str(), raw.size(), display, &rtl);
  if (display->empty())
    *display = raw;
  *is_rtl = rtl;
}

void Button::Label(const char *s) {
  SetLabel1(std::string(s));
}

void Button::SetLabel1(std::string s) {
  text1 = s;
  PrepareButtonLabel(text1, &display1, &text1_rtl);
}

void Button::SetLabel2(std::string s) {
  text2 = s;
  PrepareButtonLabel(text2, &display2, &text2_rtl);
}

void Button::SetLabel3(std::string s) {
  text3 = s;
  PrepareButtonLabel(text3, &display3, &text3_rtl);
}

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

  int line_height = ts->GetHeight();
  int text_x = bx + ((bw <= 90) ? 6 : 8);
  int right_pad = (bw <= 90) ? 6 : 8;
  if (hasIcon)
    right_pad += UiButtonSkin_IconBlockWidth(bh);
  int text_width = bw - (text_x - bx) - right_pad - 2;
  if (text_width < 6)
    text_width = 6;

  if (!text1.empty()) {
    int line_count = text3.empty() ? (text2.empty() ? 1 : 2) : 3;
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

    // For RTL labels use the display-ready (shaped + reordered) string.
    const char *src1 = text1_rtl ? display1.c_str() : text1.c_str();
    char line1[256];
    u8 len1 = ts->GetCharCountInsideWidth(src1, text.style, (u8)text_width);
    size_t bytes1 = Utf8BytesForCharCount(ts, src1, len1);
    if (bytes1 > sizeof(line1) - 1)
      bytes1 = sizeof(line1) - 1;
    memcpy(line1, src1, bytes1);
    line1[bytes1] = '\0';

    int pen1_x = text1_rtl ? RtlPenX(ts, line1, text_x, bx, bw, right_pad)
                           : text_x;
    ts->SetPen((u16)pen1_x, (u16)(top + line_height));
    ts->PrintString(line1, text.style);

    if (!text2.empty()) {
      const char *src2 = text2_rtl ? display2.c_str() : text2.c_str();
      char line2[256];
      u8 len2 =
          ts->GetCharCountInsideWidth(src2, text.style, (u8)text_width);
      size_t bytes2 = Utf8BytesForCharCount(ts, src2, len2);
      if (bytes2 > sizeof(line2) - 1)
        bytes2 = sizeof(line2) - 1;
      memcpy(line2, src2, bytes2);
      line2[bytes2] = '\0';

      int pen2_x = text2_rtl ? RtlPenX(ts, line2, text_x, bx, bw, right_pad)
                             : text_x;
      ts->SetPen((u16)pen2_x, (u16)(top + line_height * 2));
      ts->PrintString(line2, text.style);
    }

    if (!text3.empty()) {
      const char *src3 = text3_rtl ? display3.c_str() : text3.c_str();
      char line3[256];
      u8 len3 =
          ts->GetCharCountInsideWidth(src3, text.style, (u8)text_width);
      size_t bytes3 = Utf8BytesForCharCount(ts, src3, len3);
      if (bytes3 > sizeof(line3) - 1)
        bytes3 = sizeof(line3) - 1;
      memcpy(line3, src3, bytes3);
      line3[bytes3] = '\0';

      int pen3_x = text3_rtl ? RtlPenX(ts, line3, text_x, bx, bw, right_pad)
                             : text_x;
      ts->SetPen((u16)pen3_x, (u16)(top + line_height * 3));
      ts->PrintString(line3, text.style);
    }
  }

  if (hasIcon) {
    UiButtonSkin_DrawIcon(screen, stride, logicalHeight, bx, by, bw, bh,
                          resolvedIcon, !enabled);
  }

  ts->SetScreen(save_screen);
  ts->SetStyle(save_style);
}

bool Button::EnclosesPoint(u16 x, u16 y) {
  if (x > origin.x && y > origin.y && x < origin.x + extent.x &&
      y < origin.y + extent.y)
    return true;
  return false;
}

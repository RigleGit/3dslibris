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
#include "ui/button_text_layout.h"

#include <3ds.h>
#include <algorithm>
#include <vector>
#include <stdio.h>
#include <string.h>

#include "shared/text_layout_utils.h"
#include "shared/text_unicode_utils.h"
#include "shared/text_wrap_adapter.h"

namespace {

static size_t Utf8BytesForCharCount(Text *ts, const char *s, u8 count) {
  (void)ts;
  if (!s || !*s || count == 0)
    return 0;
  return text_unicode_utils::Utf8BytesForDisplayChars(s, count);
}

static u8 ButtonCharCountInsideWidth(Text *ts, const char *s, u8 style,
                                     u8 pixels) {
  // Footer labels are plain ASCII: each byte is one grapheme, so measuring
  // advances directly avoids allocating and shaping an identical run per draw.
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    if (*p < 0x20 || *p > 0x7E)
      return ts->GetCharCountInsideWidth(s, style, pixels);
  }
  unsigned int width = 0;
  u8 count = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p && count < 255; ++p) {
    width += ts->GetAdvance((u32)*p, style);
    if (width > pixels)
      break;
    ++count;
  }
  return count;
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
  wrap_ = false;
  indent = 0;
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
  if (s && text1 == s)
    return;
  SetLabel1(s ? std::string(s) : std::string());
}

void Button::SetLabel1(std::string s) {
  if (text1 == s)
    return;
  text1 = s;
  PrepareButtonLabel(text1, &display1, &text1_rtl);
}

void Button::SetLabel2(std::string s) {
  if (text2 == s)
    return;
  text2 = s;
  PrepareButtonLabel(text2, &display2, &text2_rtl);
}

void Button::SetLabel3(std::string s) {
  if (text3 == s)
    return;
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

void Button::CollectLines(int width, bool wrapping,
                          std::vector<DrawLine> *out) const {
  out->clear();
  const std::string *raw[3] = {&text1, &text2, &text3};
  const std::string *disp[3] = {&display1, &display2, &display3};
  const bool rtl[3] = {text1_rtl, text2_rtl, text3_rtl};
  if (width < 6)
    width = 6;
  for (int i = 0; i < 3; i++) {
    if (raw[i]->empty())
      continue;
    if (!wrapping) {
      // Fixed-size labels: shown as one clipped line each.
      const char *src = rtl[i] ? disp[i]->c_str() : raw[i]->c_str();
      const u8 fit = ButtonCharCountInsideWidth(ts, src, text.style, (u8)width);
      const size_t bytes = Utf8BytesForCharCount(ts, src, fit);
      DrawLine line;
      line.text.assign(src, bytes);
      line.rtl = rtl[i];
      out->push_back(line);
      continue;
    }
    // Wrap the logical string, then shape and reorder each line on its own so
    // that bidi runs never straddle a break.
    std::vector<text_wrap_utils::Line> pieces;
    text_wrap_adapter::Wrap(ts, text.style, width, *raw[i], &pieces);
    for (size_t j = 0; j < pieces.size(); j++) {
      const std::string piece =
          raw[i]->substr(pieces[j].start, pieces[j].end - pieces[j].start);
      DrawLine line;
      bool is_rtl = false;
      text_layout_utils::PrepareDisplayUtf8(piece.c_str(), piece.size(),
                                            &line.text, &is_rtl);
      if (line.text.empty())
        line.text = piece;
      line.rtl = is_rtl;
      out->push_back(line);
    }
  }
}

int Button::TextWidthFor(int box_w) const {
  const int text_x = (box_w <= 90) ? 6 : 8;
  const int right_pad = (box_w <= 90) ? 6 : 8;
  int w = box_w - text_x - right_pad - 2 - indent;
  return w < 6 ? 6 : w;
}

int Button::WrappedLineCount(int box_w) {
  if (!ts || text1.empty())
    return 0;
  const auto save_style = ts->GetStyle();
  ts->SetStyle(text.style);
  std::vector<DrawLine> lines;
  CollectLines(TextWidthFor(box_w), true, &lines);
  ts->SetStyle(save_style);
  return (int)lines.size();
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

  const int logicalWidth = ts->LogicalWidthFor(screen == ts->screenleft);
  const int logicalHeight = ScreenHeightPx(screen, ts);
  const int stride = ts->BufferStride();

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

  UiButtonSkin_Draw(screen, stride, logicalWidth, logicalHeight, bx, by, bw, bh,
                    state, hasIcon);

  const bool wrap = wrap_;
  int line_height = ts->GetHeight();
  int text_x = bx + ((bw <= 90) ? 6 : 8);
  int right_pad = (bw <= 90) ? 6 : 8;
  if (hasIcon)
    right_pad += UiButtonSkin_IconBlockWidth(bh);
  int text_width = bw - (text_x - bx) - right_pad - 2;
  if (text_width < 6)
    text_width = 6;

  const u8 saved_pixel_size = ts->GetPixelSize();
  std::vector<DrawLine> lines;
  if (!text1.empty())
    CollectLines(text_width - indent, wrap, &lines);
  if (!lines.empty()) {
    const int line_count = (int)lines.size();
    // Distance from the box top to the first baseline, and baseline pitch.
    int first_baseline;
    int pitch = line_height;
    if (line_count == 1) {
      int top = by + (bh - line_height) / 2 - 2;
      if (top < by)
        top = by;
      if (top + line_height > by + bh - 1)
        top = (by + bh - 1) - line_height;
      first_baseline = top + line_height;
    } else {
      // The skin's border and drop shadow take button_text_layout::kInset
      // at the top and the bottom, so multi-line text is laid out inside. Lines are
      // centered as a block; if the block does not fit at the natural line
      // pitch the pitch tightens, and as a last resort the text shrinks.
      button_text_layout::Block block;
      for (;;) {
        line_height = ts->GetHeight();
        block = button_text_layout::Compute(bh, line_count, line_height,
                                            ts->GetAscent(), ts->GetDescent());
        const u8 size = ts->GetPixelSize();
        if (block.fits || size <= 8)
          break;
        ts->SetPixelSize((u8)(size - 1));
      }
      pitch = block.pitch;
      first_baseline = by + block.first_baseline;
    }

    const int line_x = text_x + indent;
    for (int i = 0; i < line_count; i++) {
      const DrawLine &line = lines[(size_t)i];
      const int pen_x = line.rtl ? RtlPenX(ts, line.text.c_str(), line_x, bx,
                                           bw, right_pad)
                                 : line_x;
      ts->SetPen((u16)pen_x, (u16)(first_baseline + i * pitch));
      ts->PrintString(line.text.c_str(), text.style);
    }
  }

  if (hasIcon) {
    UiButtonSkin_DrawIcon(screen, stride, logicalWidth, logicalHeight, bx, by,
                          bw, bh, resolvedIcon, !enabled);
  }

  ts->SetPixelSize(saved_pixel_size);
  ts->SetScreen(save_screen);
  ts->SetStyle(save_style);
}

bool Button::EnclosesPoint(u16 x, u16 y) {
  if (x > origin.x && y > origin.y && x < origin.x + extent.x &&
      y < origin.y + extent.y)
    return true;
  return false;
}

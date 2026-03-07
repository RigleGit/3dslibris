/*

dslibris - an ebook reader for the Nintendo DS.

 Copyright (C) 2007-2008 Ray Haleblian (ray23@sourceforge.net)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

*/

#include "button.h"
#include <3ds.h>
#include <stdio.h>

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
  ts = typesetter;
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

void Button::Draw(u16 *screen, bool highlight) {
  // push state
  auto save_screen = ts->GetScreen();
  auto save_style = ts->GetStyle();

  u16 x, y;
  coord_t ul, lr;
  ul.x = origin.x;
  ul.y = origin.y;
  lr.x = origin.x + extent.x;
  lr.y = origin.y + extent.y;
  int w = ts->display.height; // no really

  if (screen == nullptr)
    screen = ts->screen;

  ts->SetScreen(screen);
  ts->SetStyle(text.style);

  // Selected items get a light blue tint, unselected get white
  u16 bgcolor = highlight ? 0xDF9E /* light cyan */ : 0xFFFF /* white */;
  for (y = ul.y; y < lr.y; y++) {
    for (x = ul.x; x < lr.x; x++) {
      screen[y * w + x] = bgcolor;
    }
  }

  if (highlight) {
    // 3px thick black border for selected item
    u16 bordercolor = 0x0000;
    for (int t = 0; t < 3; t++) {
      for (int x = ul.x; x < lr.x; x++) {
        screen[(ul.y + t) * w + x] = bordercolor;
        screen[(lr.y - 1 - t) * w + x] = bordercolor;
      }
      for (int y = ul.y; y < lr.y; y++) {
        screen[y * w + ul.x + t] = bordercolor;
        screen[y * w + lr.x - 1 - t] = bordercolor;
      }
    }
  } else {
    // 1px light gray border for unselected
    u16 bordercolor = 0xBDD7;
    for (int x = ul.x; x < lr.x; x++) {
      screen[ul.y * w + x] = bordercolor;
      screen[(lr.y - 1) * w + x] = bordercolor;
    }
    for (int y = ul.y; y < lr.y; y++) {
      screen[y * w + ul.x] = bordercolor;
      screen[y * w + lr.x - 1] = bordercolor;
    }
  }

  if (text1.length()) {
    ts->SetPen(ul.x + 6, ul.y + ts->GetHeight());
    int text_width = (lr.x - ul.x - 8);
    u8 len =
        ts->GetCharCountInsideWidth(text1.c_str(), text.style, text_width);
    size_t bytes = Utf8BytesForCharCount(ts, text1.c_str(), len);
    ts->PrintString(text1.substr(0, bytes).c_str(), text.style);
  }

  if (text2.length()) {
    ts->SetPen(ul.x + 6, ts->GetPenY() + ts->GetHeight());
    int text_width = (lr.x - ul.x - 8);
    u8 len2 =
        ts->GetCharCountInsideWidth(text2.c_str(), text.style, text_width);
    size_t bytes2 = Utf8BytesForCharCount(ts, text2.c_str(), len2);
    ts->PrintString(text2.substr(0, bytes2).c_str(), text.style);
  }

  // pop state
  ts->SetScreen(save_screen);
  ts->SetStyle(save_style);
}

bool Button::EnclosesPoint(u16 x, u16 y) {
  if (x > origin.x && y > origin.y && x < origin.x + extent.x &&
      y < origin.y + extent.y)
    return true;
  return false;
}

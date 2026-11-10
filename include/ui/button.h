/*
    3dslibris - button.h
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Touchable UI button model used across browser/settings/list menus.
    - Supports two-line labels, UTF-8 truncation and style-specific drawing.
    - Adds optional icon mapping through procedural button skin renderer.
*/

#pragma once

#include <string>
#include <vector>
#include <unistd.h>

#include "ui/text.h"
#include "ui/ui_button_skin.h"

enum {
  BUTTON_STYLE_SETTING,
  BUTTON_STYLE_BOOK
};

typedef struct {
  u16 x;
  u16 y;
} coord_t;

class Button {
private:
  coord_t origin;
  coord_t extent;
  struct {
    int pixelsize;
    int style;
  } text;
  int style;
  std::string text1;
  std::string text2;
  std::string text3;
  // Display-ready strings: Arabic shaped + BIDI reordered.
  std::string display1;
  std::string display2;
  std::string display3;
  bool text1_rtl;
  bool text2_rtl;
  bool text3_rtl;
  UiButtonIconId icon;
  bool enabled;
  bool iconExplicit;
  bool wrap_;
  int indent;
  Text *ts;

  struct DrawLine {
    std::string text; // display-ready: shaped and reordered for RTL
    bool rtl;
  };
  void CollectLines(int width, bool wrapping, std::vector<DrawLine> *out) const;
  int TextWidthFor(int box_w) const;

  UiButtonIconId ResolveIcon() const;

public:
  Button();
  Button(Text *typesetter);
  inline void Init() { Init(ts); };
  void Init(Text *typesetter);
  inline int GetHeight() { return extent.y; };
  inline int GetX() const { return origin.x; };
  inline int GetY() const { return origin.y; };
  inline const char *GetLabel() { return text1.c_str(); };
  void Label(const char *text);
  inline void SetLabel(std::string &s) { SetLabel1(s); };
  void SetLabel1(std::string s);
  void SetLabel2(std::string s);
  void SetLabel3(std::string s);
  inline void SetIcon(UiButtonIconId iconId) {
    icon = iconId;
    iconExplicit = true;
  };
  inline void ClearIcon() {
    icon = UI_BUTTON_ICON_NONE;
    iconExplicit = false;
  };
  inline UiButtonIconId GetIcon() const { return icon; };
  // Wrapped buttons break every label onto as many lines as it needs instead
  // of clipping it to one line; the caller sizes the box with
  // WrappedLineCount() so nothing is dropped.
  inline void SetWrap(bool on) { wrap_ = on; }
  // Extra left padding for the text (hanging indent of nested list entries).
  inline void SetIndent(int px) { indent = px; }
  // Lines the labels occupy when wrapped for a box `box_w` pixels wide.
  int WrappedLineCount(int box_w);
  inline void SetEnabled(bool isEnabled) { enabled = isEnabled; };
  inline bool IsEnabled() const { return enabled; };
  inline void SetStyle(int astyle) { style = astyle; };
  void Draw(u16 *fb, bool highlight = false);
  inline void Draw(bool highlight = false) { Draw(ts->GetScreen(), highlight); }
  void Move(u16 x, u16 y);
  void Resize(u16 x, u16 y);
  bool EnclosesPoint(u16 x, u16 y);
};

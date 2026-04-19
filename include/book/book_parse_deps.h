#pragma once

#include <string>

#include "book/book.h"
#include "ui/text.h"

struct TextLayoutSnapshot {
  int pixel_size;
  int linespacing;
  int margin_left;
  int margin_right;
  int margin_top;
  int margin_bottom;
  std::string regular_font_path;

  TextLayoutSnapshot()
      : pixel_size(0), linespacing(0), margin_left(0), margin_right(0),
        margin_top(0), margin_bottom(0), regular_font_path("") {}
};

inline TextLayoutSnapshot CaptureTextLayoutSnapshot(Text *ts) {
  TextLayoutSnapshot snap;
  if (!ts)
    return snap;
  snap.pixel_size = (int)ts->GetPixelSize();
  snap.linespacing = ts->linespacing;
  snap.margin_left = (int)ts->margin.left;
  snap.margin_right = (int)ts->margin.right;
  snap.margin_top = (int)ts->margin.top;
  snap.margin_bottom = (int)ts->margin.bottom;
  snap.regular_font_path = ts->GetFontFile(TEXT_STYLE_REGULAR);
  return snap;
}

struct BookParseDeps {
  IStatusReporter *reporter;
  Text *ts;
  TextLayoutSnapshot layout;
  Prefs *prefs;
  std::string regular_font_path;
  int paragraph_spacing;
  int paragraph_indent;
  int orientation;

  BookParseDeps()
      : reporter(NULL), ts(NULL), layout(), regular_font_path(""), paragraph_spacing(0),
        paragraph_indent(0), orientation(0) {}
};

inline BookParseDeps BuildBookParseDeps(Book *book) {
  BookParseDeps deps;
  deps.reporter = book ? book->GetStatusReporter() : NULL;
  deps.ts = book ? book->GetText() : NULL;
  deps.layout = CaptureTextLayoutSnapshot(deps.ts);
  deps.prefs = book ? book->GetPrefs() : NULL;
  deps.regular_font_path =
      book && book->GetText()
          ? book->GetText()->GetFontFile(TEXT_STYLE_REGULAR)
          : "";
  deps.paragraph_spacing = book ? book->GetParagraphSpacing() : 0;
  deps.paragraph_indent = book ? book->GetParagraphIndent() : 0;
  deps.orientation = book ? book->GetOrientation() : 0;
  return deps;
}

#pragma once

#include "shared/orientation_utils.h"

namespace reader {

// Global reader defaults. Book-specific overrides remain owned by Book;
// BookContext borrows these stable fields when resolving its effective layout.
struct ReaderLayoutState {
  int font_size;
  int line_spacing;
  unsigned char paragraph_spacing;
  unsigned char paragraph_indent;
  unsigned char orientation;
  bool publisher_text_indent;
  bool publisher_block_margins;
  bool publisher_horizontal_margins;

  ReaderLayoutState()
      : font_size(12), line_spacing(0), paragraph_spacing(0),
        paragraph_indent(0), orientation(orientation_utils::ORIENT_TURNED_LEFT),
        publisher_text_indent(true), publisher_block_margins(true),
        publisher_horizontal_margins(true) {}
};

} // namespace reader

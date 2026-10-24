#pragma once

class Book {
public:
  struct MuPdfState;
  MuPdfState *mupdf_state;
  bool IsPdf() const { return true; }
  bool MoveMuPdfViewportToPreview(int touch_x, int touch_y);
};

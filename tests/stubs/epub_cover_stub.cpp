/*
 * Stub for formats/epub/epub_cover.cpp.
 * Cover extraction needs png.h and MuPDF (platform rendering deps).
 * The Open/parse path never calls Extract; FindLikelyImagePath is called
 * from epub_manifest.cpp but a no-find result is safe for a fixture with
 * no cover image declared.
 */
#include "formats/epub/epub_cover.h"

namespace epub_cover {

int Extract(Book *, const std::string &) { return -1; }

bool FindLikelyImagePath(epub_data_t &, const std::string &,
                         std::string &) {
  return false;
}

} // namespace epub_cover

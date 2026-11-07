#include "book/book.h"
#include "shared/cover_decode_utils.h"
#include "formats/epub/epub_cover.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
extern "C" {
#include "mupdf/fitz.h"
}
static size_t largest_stbi_allocation = 0;
void *CoverMalloc(size_t n) {
  largest_stbi_allocation = std::max(n, largest_stbi_allocation);
  return std::malloc(n);
}
void *CoverRealloc(void *p, size_t n) {
  largest_stbi_allocation = std::max(n, largest_stbi_allocation);
  return std::realloc(p, n);
}
void CoverFree(void *p) { std::free(p); }
static void NoLock(void *, int) {}
fz_locks_context g_mupdf_locks_ctx = {nullptr, NoLock, NoLock};
void InitMuPdfLocks() {}

int main(int argc, char **argv) {
  assert(argc == 3);
  std::ifstream input(argv[1], std::ios::binary);
  const std::vector<unsigned char> image((std::istreambuf_iterator<char>(input)), {});
  assert(!image.empty());
  const unsigned char *bytes = image.data();
  const size_t size = image.size();
  Book book;
  assert(cover_decode_utils::DecodeImageToCoverThumb(&book, bytes, size, 4096));
  assert(book.coverWidth == 76 && book.coverHeight == 115);
  for (int i = 0; i < book.coverWidth * book.coverHeight; ++i)
    assert(((book.coverPixels[i] >> 5) & 63) > 50 &&
           (book.coverPixels[i] >> 11) < 4);
  assert(largest_stbi_allocation < 128u * 1024u &&
         "3DS cover decode must not expand the full JPEG through stb");
  u16 *previous = book.coverPixels;
  assert(!cover_decode_utils::DecodeImageToCoverThumb(&book, bytes, size, 2048));
  assert(book.coverPixels == previous && book.coverWidth == 76);
  const unsigned char corrupt[] = {0, 1, 2};
  assert(!cover_decode_utils::DecodeImageToCoverThumb(&book, corrupt, 3));
  assert(book.coverPixels == previous);
  // Exercise EPUB's actual archive/preflight/decoder path. A 4800x6400 JPEG
  // must produce a bounded thumbnail, while the PNG dimension policy remains.
  Book epub;
  epub.coverImagePath = "cover.jpg";
  assert(epub_cover::Extract(&epub, argv[2]) == 0);
  assert(epub.coverWidth == 85 && epub.coverHeight == 113);
  for (int i = 0; i < epub.coverWidth * epub.coverHeight; ++i)
    assert(((epub.coverPixels[i] >> 5) & 63) > 50 &&
           (epub.coverPixels[i] >> 11) < 4);
  assert(largest_stbi_allocation < 128u * 1024u);
  epub.coverImagePath = "oversized.png";
  previous = epub.coverPixels;
  assert(epub_cover::Extract(&epub, argv[2]) == 7);
  assert(epub.coverPixels == previous);
  epub.coverImagePath = "oversized.jpg";
  assert(epub_cover::Extract(&epub, argv[2]) == 7);
  assert(epub.coverPixels == previous);
  puts("PASS: large JPEG cover subsampling, pixels and failure ownership");
}

#pragma once

#include <3ds.h>

#include <cstddef>

class Book;

namespace cover_decode_utils {

// Decode an image buffer (JPEG/PNG/GIF/BMP via stb_image), scale to fit
// inside the browser cover-thumbnail box preserving aspect, and assign the
// resulting RGB565 pixels to book->coverPixels (replacing any prior buffer).
//
// Returns true on success, false on decode failure or out-of-policy
// dimensions. max_dimension caps the source image size before decode so a
// malformed or oversized image cannot exhaust the heap; pass 0 to skip the
// cap. Used by every reflowable format with embedded raster covers (MOBI,
// FB2, EPUB raster fallback). PDF/CBZ have their own pipelines because they
// already work in RGB565.
bool DecodeImageToCoverThumb(Book *book, const unsigned char *data,
                             std::size_t size, int max_dimension = 2048);

} // namespace cover_decode_utils

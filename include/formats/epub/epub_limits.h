/*
    3dslibris - epub_limits.h
    EPUB format constants and size limits.
    Extracted from epub.cpp by Rigle.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace epub_limits {

static const size_t kTocMaxBytes = 192 * 1024;
static const size_t kTocMaxEntries = 2048;
static const size_t kContentMaxBytes = 12 * 1024 * 1024;
static const size_t kCoverMaxEntryBytes = 8 * 1024 * 1024;
static const size_t kCoverMaxNonJpegBytes = 2 * 1024 * 1024;
static const size_t kCoverMaxDecodedRgbBytes = 16 * 1024 * 1024;
static const size_t kSvgWrapperMaxBytes = 512 * 1024;
static const int kCoverMaxDimension = 4096;

static const bool kEnableRealTocResolve = true;

// Flowed EPUB pages are retained as resident Page buffers while reading.
// Keep this below the 3DS crash zone for very large anthologies.
static const uint16_t kMaxPagesInMemory = 25000;

} // namespace epub_limits

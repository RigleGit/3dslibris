#pragma once
// Build-time switches for the zoomed-PDF tile mosaic work. Each defaults to ON;
// define the macro to 0 (for example -DDSLIBRIS_FIXED_LAYOUT_HW_UPLOAD=0) to
// restore the previous behaviour without touching any other code.

// 1: render MuPDF tiles into linear memory and let the GPU transfer engine
//    (GX display transfer, RGB8 -> tiled RGB565) fill the texture. 0: render
//    the same tile and swizzle it on the CPU.
// Default OFF: the first hardware run of the mosaic (Old 3DS, 400px) froze
// the console with no further log line, consistent with a display transfer
// whose completion never arrives. Azahar accepts it; real hardware has not.
#ifndef DSLIBRIS_FIXED_LAYOUT_HW_UPLOAD
#define DSLIBRIS_FIXED_LAYOUT_HW_UPLOAD 0
#endif

// 2: zoomed synchronous PDF panning uses a cache of fixed 256x256 GPU tiles
//    instead of one large re-rasterized interactive tile. 0: previous path.
#ifndef DSLIBRIS_FIXED_LAYOUT_MOSAIC
#define DSLIBRIS_FIXED_LAYOUT_MOSAIC 1
#endif

// 3a: with the mosaic active, present the sharp top screen first and render
//     the bottom-screen overview later, when input is idle.
#ifndef DSLIBRIS_FIXED_LAYOUT_DEFER_PREVIEW
#define DSLIBRIS_FIXED_LAYOUT_DEFER_PREVIEW 1
#endif

// 3b: keep the previous page's display list while the heap has headroom.
#ifndef DSLIBRIS_FIXED_LAYOUT_KEEP_PREV_LIST
#define DSLIBRIS_FIXED_LAYOUT_KEEP_PREV_LIST 1
#endif

namespace fixed_layout_mosaic {
constexpr bool kHardwareUpload = DSLIBRIS_FIXED_LAYOUT_HW_UPLOAD != 0;
constexpr bool kEnabled = DSLIBRIS_FIXED_LAYOUT_MOSAIC != 0;
constexpr bool kDeferPreview = DSLIBRIS_FIXED_LAYOUT_DEFER_PREVIEW != 0;
constexpr bool kKeepPreviousList = DSLIBRIS_FIXED_LAYOUT_KEEP_PREV_LIST != 0;
} // namespace fixed_layout_mosaic

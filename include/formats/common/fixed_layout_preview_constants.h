#pragma once

#ifdef __3DS__
#include <3ds.h>
#else
#include <stdint.h>
typedef uint16_t u16;
#endif

namespace fixed_layout_preview {

static const int kPadding = 4;
static const u16 kPaper = 0xFFFF;
static const u16 kFrame = 0x2104;
static const u16 kViewportAccent = 0x0000;

} // namespace fixed_layout_preview

/*
    3dslibris - screen_dimensions.h
    Single source of truth for physical 3DS screen pixel dimensions.
    Use this header when code needs raw pixel counts for either screen.
    Do not add UI layout constants here — those belong in screen_layout_constants.h.
*/

#pragma once

namespace screen_dims {

// Physical pixel dimensions of the two 3DS screens in landscape mode.
// "Height" here is the longer axis (left/right edge), matching buffer layout.
static const int kTopScreenWidthPx     = 240;
static const int kTopScreenHeightPx    = 400;
static const int kBottomScreenWidthPx  = 240;
static const int kBottomScreenHeightPx = 320;

} // namespace screen_dims

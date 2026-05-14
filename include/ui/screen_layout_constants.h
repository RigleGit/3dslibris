/*
    3dslibris - screen_layout_constants.h
    New 3DS reader module by Rigle.

    Summary:
    - Shared bottom-screen footer button layout constants.
    - Used by browser and settings controllers for the three-button nav row.
*/

#pragma once

namespace screen_layout {

// Physical pixel heights of the two 3DS screens in landscape mode.
static const int kTopScreenHeightPx    = 400;
static const int kBottomScreenHeightPx = 320;

static const int kFooterY       = 296;
static const int kFooterButtonH = 22;
static const int kFooterNavW    = 66;
static const int kFooterMidW    = 96;
static const int kFooterLeftX   = 2;
static const int kFooterMidX    = 72;
static const int kFooterRightX  = 172;

} // namespace screen_layout

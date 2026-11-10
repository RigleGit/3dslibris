#pragma once

#include <stdint.h>

#include "reader/fixed_layout_input_utils.h"

// Resolved key masks for the reader. ReaderControls is built from a control
// scheme table (reader/reader_control_scheme.h) written in logical buttons and
// resolved through the active KeyMap, so the L/R and ZL/ZR handedness swap
// applied by App::ApplyRenderOrientation is preserved. Handlers consume only
// these masks and never read raw app.key.* for reader actions.

struct ReaderControls
{
  // Reflowable page navigation
  uint32_t page_next;
  uint32_t page_prev;
  // Keys that auto-repeat page turns (subset of page_next/page_prev)
  uint32_t page_repeat_next_dpad;
  uint32_t page_repeat_prev_dpad;
  uint32_t page_repeat_next_circle;
  uint32_t page_repeat_prev_circle;
  // Bookmark navigation
  uint32_t bookmark_next;
  uint32_t bookmark_prev;
  // Inline link focus (EPUB only): sequential, crosses pages
  uint32_t link_next;
  uint32_t link_prev;
  // Fixed-layout (PDF/CBZ) page and chapter navigation
  uint32_t fixed_page_next;
  uint32_t fixed_page_prev;
  uint32_t fixed_chapter_next;
  uint32_t fixed_chapter_prev;
  // Zoom (fixed layout only)
  uint32_t zoom_in;
  uint32_t zoom_out;
  // Global reader actions
  uint32_t back_to_library;
  uint32_t open_settings;
  // Reflowable: tap toggles a bookmark, hold enters inline link focus
  uint32_t focus_hold;
  // Inline link focus: open/follow the focused link, leave focus, follow
  // image links only
  uint32_t link_activate;
  uint32_t link_exit;
  uint32_t link_follow_image;
  // Reflowable color mode cycle
  uint32_t color_cycle;
  // Fixed-layout shoulder page turns (PDF/CBZ)
  uint32_t fixed_shoulder_next;
  uint32_t fixed_shoulder_prev;
};

#include "reader/reader_control_scheme.h"

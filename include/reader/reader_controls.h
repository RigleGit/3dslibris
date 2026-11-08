#pragma once

#include <stdint.h>

#include "reader/fixed_layout_input_utils.h"

// Portrait-orientation control mapping for the reflowable reader.
// All DPad assignments assume the device is held in portrait (rotated clockwise):
//   dleft  = physically down on screen = reading forward
//   dright = physically up on screen   = reading backward

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
};

template <typename KeyMap>
inline ReaderControls BuildPortraitControls(const KeyMap &key)
{
  ReaderControls c;
  c.page_next = reader_input_utils::ReflowableNextPageKeys(
      key.a, key.r, key.down, key.ddown, key.zl);
  c.page_prev = reader_input_utils::ReflowablePrevPageKeys(
      key.b, key.l, key.up, key.dup, key.zr);
  c.page_repeat_next_dpad = key.ddown;
  c.page_repeat_prev_dpad = key.dup;
  c.page_repeat_next_circle = key.down;
  c.page_repeat_prev_circle = key.up;
  c.bookmark_next = reader_input_utils::ReflowableBookmarkNextKeys(
      key.left, key.dleft);
  c.bookmark_prev = reader_input_utils::ReflowableBookmarkPrevKeys(
      key.right, key.dright);
  // dleft = physically down in portrait = forward; dright = up = backward
  c.link_next = key.ddown | key.dleft;
  c.link_prev = key.dup | key.dright;
  c.fixed_page_next = key.dright;
  c.fixed_page_prev = key.dleft;
  c.fixed_chapter_next = key.ddown;
  c.fixed_chapter_prev = key.dup;
  c.zoom_in = key.a;
  c.zoom_out = key.b;
  c.back_to_library = key.start;
  c.open_settings = key.select;
  return c;
}

template <typename KeyMap>
inline ReaderControls BuildLandscapeControls(const KeyMap &key)
{
  ReaderControls c;
  c.page_next = key.a | key.r | key.right | key.dright | key.zl;
  c.page_prev = key.b | key.l | key.left | key.dleft | key.zr;
  c.page_repeat_next_dpad = key.dright;
  c.page_repeat_prev_dpad = key.dleft;
  c.page_repeat_next_circle = key.right;
  c.page_repeat_prev_circle = key.left;
  c.bookmark_next = key.down | key.ddown;
  c.bookmark_prev = key.up | key.dup;
  c.link_next = key.down | key.ddown;
  c.link_prev = key.up | key.dup;
  c.fixed_page_next = key.dright;
  c.fixed_page_prev = key.dleft;
  c.fixed_chapter_next = key.ddown;
  c.fixed_chapter_prev = key.dup;
  c.zoom_in = key.a;
  c.zoom_out = key.b;
  c.back_to_library = key.start;
  c.open_settings = key.select;
  return c;
}

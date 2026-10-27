#pragma once

#include <algorithm>

#include "shared/orientation_utils.h"

namespace text_render_layout_utils {

static const int kFullReadingScreenFooterGuardPx = 8;
// Landscape reading reserves a slim HUD strip at the bottom of the second
// (bottom) screen for clock/progress.
static const int kLandscapeHudStripPx = 22;
static const int kLandscapeReadingScreenHeightPx = 240;

// Returns the pixel height of a reading screen by its 0-based index.
// Screen 0 is the first/left screen (400 px); screen 1 is the second/right
// screen (320 px). Assumes normal (non-turned) device orientation.
inline int ReadingScreenHeightPx(int reading_screen_index) {
  return reading_screen_index == 0 ? 400 : 320;
}

struct ReadingScreenMetrics {
  int max_height;
  int bottom_margin;
};

inline int ResolvePaginationSafetyGuard(int line_height) {
  return std::max(kFullReadingScreenFooterGuardPx, line_height / 2);
}

inline int ApplyLineHeightPaginationGuard(int guarded_bottom_margin,
                                          int line_height) {
  return guarded_bottom_margin +
         (ResolvePaginationSafetyGuard(line_height) -
          kFullReadingScreenFooterGuardPx);
}

inline int ComputeRtlLineStartX(int margin_left, int content_right,
                                int line_width) {
  const int x = content_right - std::max(0, line_width);
  return std::max(margin_left, x);
}

inline bool ShouldAutoWrapGlyph(bool auto_wrap_enabled, bool is_browser_style,
                                int pen_x, int margin_left, int glyph_right,
                                int content_right) {
  if (!auto_wrap_enabled || is_browser_style)
    return false;
  if (pen_x <= margin_left)
    return false;
  return glyph_right > content_right;
}

inline int ResolveClipRight(int screen_width, int content_right,
                            bool clip_to_content_enabled) {
  if (!clip_to_content_enabled)
    return screen_width;
  return std::max(0, std::min(screen_width, content_right));
}

inline ReadingScreenMetrics ResolveReadingScreenMetrics(
    bool on_first_screen, bool first_screen_is_left, int left_bottom_margin,
    int right_bottom_margin) {
  const bool current_screen_is_left =
      on_first_screen ? first_screen_is_left : !first_screen_is_left;

  ReadingScreenMetrics metrics{};
  metrics.max_height = current_screen_is_left ? 400 : 320;
  metrics.bottom_margin =
      (current_screen_is_left ? left_bottom_margin : right_bottom_margin) +
      kFullReadingScreenFooterGuardPx;
  return metrics;
}

inline int ResolveCompactReadingBottomMargin(int full_bottom_margin) {
  return (full_bottom_margin > 20) ? 20 : full_bottom_margin;
}

// Pixel clip margin used by TextRenderer for a reading screen. This excludes
// the extra overflow guard used by the paginator. In landscape, screen 0 has
// no HUD and may draw to the physical edge; screen 1 reserves the HUD strip.
inline int ResolveReadingScreenRenderBottomMarginForOrientation(
    unsigned char orientation, int current_screen, int left_bottom_margin,
    int right_bottom_margin) {
  if (orientation_utils::IsLandscape(orientation)) {
    return current_screen == 0 ? 0 : kLandscapeHudStripPx;
  }

  const bool first_screen_is_left =
      !orientation_utils::IsTurnedRight(orientation);
  const bool current_screen_is_left =
      current_screen == 0 ? first_screen_is_left : !first_screen_is_left;
  return current_screen_is_left ? left_bottom_margin : right_bottom_margin;
}

inline ReadingScreenMetrics ResolveReadingScreenMetricsForReadingScreen(
    bool turned_right, int current_screen, int left_bottom_margin,
    int right_bottom_margin) {
  const bool on_first_screen = (current_screen == 0);
  const bool first_screen_is_left = !turned_right;
  return ResolveReadingScreenMetrics(on_first_screen, first_screen_is_left,
                                     left_bottom_margin, right_bottom_margin);
}

// Orientation-aware variant. Landscape reads top (screen 0) then bottom
// (screen 1). The top screen has no HUD and reserves only the pagination
// guard; the bottom screen reserves the HUD strip plus the same guard.
inline ReadingScreenMetrics ResolveReadingScreenMetricsForOrientation(
    unsigned char orientation, int current_screen, int left_bottom_margin,
    int right_bottom_margin) {
  if (orientation_utils::IsLandscape(orientation)) {
    ReadingScreenMetrics metrics{};
    metrics.max_height = kLandscapeReadingScreenHeightPx;
    metrics.bottom_margin =
        (current_screen == 0)
            ? kFullReadingScreenFooterGuardPx
            : (kLandscapeHudStripPx + kFullReadingScreenFooterGuardPx);
    return metrics;
  }
  return ResolveReadingScreenMetricsForReadingScreen(
      orientation_utils::IsTurnedRight(orientation), current_screen,
      left_bottom_margin, right_bottom_margin);
}

inline bool WouldOverflowReadingScreen(int pen_y, int line_height,
                                       int line_spacing, int max_height,
                                       int bottom_margin) {
  return (pen_y + line_height + line_spacing) > (max_height - bottom_margin);
}

inline bool HasRoomForFollowingLine(int pen_y, int line_height,
                                    int line_spacing, int max_height,
                                    int bottom_margin) {
  return !WouldOverflowReadingScreen(pen_y, line_height, line_spacing,
                                     max_height, bottom_margin);
}

inline bool CurrentLineFitsScreen(int pen_y, int line_height,
                                  int line_spacing, int max_height,
                                  int bottom_margin) {
  (void)line_height;
  (void)line_spacing;
  return pen_y <= (max_height - bottom_margin);
}

inline bool CurrentLineBeyondReadingScreen(int pen_y, int max_height,
                                           int bottom_margin) {
  return pen_y > (max_height - bottom_margin);
}

inline bool ShouldAdvanceAfterBandImage(int pen_y, int max_height,
                                        int bottom_margin) {
  return CurrentLineBeyondReadingScreen(pen_y, max_height, bottom_margin);
}

inline bool ShouldAdvanceParagraphStartGuard(bool current_line_fits,
                                             bool has_room_for_following_line,
                                             bool upcoming_fits_one_line) {
  return current_line_fits && !has_room_for_following_line &&
         !upcoming_fits_one_line;
}

} // namespace text_render_layout_utils

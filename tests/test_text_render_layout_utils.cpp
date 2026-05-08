#include "shared/text_render_layout_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  std::fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void TestComputeRtlLineStartX() {
  ExpectEq("anchors to right edge", text_render_layout_utils::ComputeRtlLineStartX(12, 228, 40), 188);
  ExpectEq("clamps to left margin", text_render_layout_utils::ComputeRtlLineStartX(12, 228, 400), 12);
  ExpectEq("empty line keeps right edge", text_render_layout_utils::ComputeRtlLineStartX(12, 228, 0), 228);
}

void TestShouldAutoWrapGlyph() {
  const int margin_left = 12;
  const int content_right = 228;
  const int glyph_right_overflow = 232;
  const int glyph_right_fit = 220;

  ExpectTrue("wraps when enabled and overflowing",
             text_render_layout_utils::ShouldAutoWrapGlyph(
                 true, false, 100, margin_left, glyph_right_overflow,
                 content_right));
  ExpectFalse("no wrap when disabled",
              text_render_layout_utils::ShouldAutoWrapGlyph(
                  false, false, 100, margin_left, glyph_right_overflow,
                  content_right));
  ExpectFalse("no wrap in browser style",
              text_render_layout_utils::ShouldAutoWrapGlyph(
                  true, true, 100, margin_left, glyph_right_overflow,
                  content_right));
  ExpectFalse("no wrap at left margin start",
              text_render_layout_utils::ShouldAutoWrapGlyph(
                  true, false, margin_left, margin_left, glyph_right_overflow,
                  content_right));
  ExpectFalse("no wrap when glyph fits",
              text_render_layout_utils::ShouldAutoWrapGlyph(
                  true, false, 100, margin_left, glyph_right_fit,
                  content_right));
}

void TestResolveClipRight() {
  ExpectEq("clip disabled uses full screen",
           text_render_layout_utils::ResolveClipRight(240, 228, false), 240);
  ExpectEq("clip enabled uses content right",
           text_render_layout_utils::ResolveClipRight(240, 228, true), 228);
  ExpectEq("clip enabled clamps content right high",
           text_render_layout_utils::ResolveClipRight(240, 300, true), 240);
  ExpectEq("clip enabled clamps content right low",
           text_render_layout_utils::ResolveClipRight(240, -3, true), 0);
}

void TestResolveReadingScreenMetrics() {
  text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetrics(
          true, true, 36, 16);
  ExpectEq("first left height", metrics.max_height, 400);
  ExpectEq("first left bottom margin", metrics.bottom_margin, 44);

  metrics = text_render_layout_utils::ResolveReadingScreenMetrics(
      false, true, 36, 16);
  ExpectEq("second right height", metrics.max_height, 320);
  ExpectEq("second right bottom margin", metrics.bottom_margin, 16);

  metrics = text_render_layout_utils::ResolveReadingScreenMetrics(
      true, false, 36, 16);
  ExpectEq("first right height", metrics.max_height, 320);
  ExpectEq("first right bottom margin", metrics.bottom_margin, 16);

  metrics = text_render_layout_utils::ResolveReadingScreenMetrics(
      false, false, 36, 16);
  ExpectEq("second left height", metrics.max_height, 400);
  ExpectEq("second left bottom margin", metrics.bottom_margin, 44);
}

void TestResolveReadingScreenMetricsForReadingScreen() {
  text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          false, 0, 36, 16);
  ExpectEq("turned-left first screen height", metrics.max_height, 400);
  ExpectEq("turned-left first screen bottom margin", metrics.bottom_margin, 44);

  metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          false, 1, 36, 16);
  ExpectEq("turned-left second screen height", metrics.max_height, 320);
  ExpectEq("turned-left second screen bottom margin", metrics.bottom_margin,
           16);

  metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          true, 0, 36, 16);
  ExpectEq("turned-right first screen height", metrics.max_height, 320);
  ExpectEq("turned-right first screen bottom margin", metrics.bottom_margin,
           16);

  metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          true, 1, 36, 16);
  ExpectEq("turned-right second screen height", metrics.max_height, 400);
  ExpectEq("turned-right second screen bottom margin", metrics.bottom_margin,
           44);
}

void TestResolveCompactReadingBottomMargin() {
  ExpectEq("keeps compact footer when already small",
           text_render_layout_utils::ResolveCompactReadingBottomMargin(12), 12);
  ExpectEq("caps compact footer to 20",
           text_render_layout_utils::ResolveCompactReadingBottomMargin(36), 20);
}

void TestWouldOverflowReadingScreen() {
  ExpectFalse("line fits with spacing",
              text_render_layout_utils::WouldOverflowReadingScreen(
                  340, 12, 1, 400, 36));
  ExpectTrue("line overflows once spacing is counted",
             text_render_layout_utils::WouldOverflowReadingScreen(
                 352, 12, 1, 400, 36));
  ExpectFalse("line may sit exactly on footer edge",
              text_render_layout_utils::WouldOverflowReadingScreen(
                  352, 12, 0, 400, 36));
}

void TestWouldCurrentLineOverflowReadingScreen() {
  ExpectFalse("current parser line may sit exactly on footer edge",
              text_render_layout_utils::WouldOverflowReadingScreen(
                  364, 0, 0, 400, 36));
  ExpectTrue("current parser line overflows below footer edge",
             text_render_layout_utils::WouldOverflowReadingScreen(
                 365, 0, 0, 400, 36));
}

void TestCurrentLineVisibilityIgnoresFollowingLine() {
  const int max_height = 320;
  const int bottom_margin = 16;
  const int threshold = max_height - bottom_margin;

  ExpectFalse("last visible current line is still valid",
              text_render_layout_utils::CurrentLineBeyondReadingScreen(
                  threshold, max_height, bottom_margin));
  ExpectTrue("line below visible threshold is invalid",
             text_render_layout_utils::CurrentLineBeyondReadingScreen(
                 threshold + 1, max_height, bottom_margin));
}

void TestParagraphStartGuardAllowsOneLineParagraphInLastSlot() {
  ExpectFalse("one-line paragraph may use last visible slot",
              text_render_layout_utils::ShouldAdvanceParagraphStartGuard(
                  true, false, true));
  ExpectTrue("long paragraph advances when only one slot remains",
             text_render_layout_utils::ShouldAdvanceParagraphStartGuard(
                 true, false, false));
  ExpectFalse("two slots remaining does not advance",
              text_render_layout_utils::ShouldAdvanceParagraphStartGuard(
                  true, true, false));
}

void TestCurrentLineFitAllowsVisualLastLineWithoutFollowingRoom() {
  const int max_height = 320;
  const int bottom_margin = 16;
  const int line_h = 13;
  const int line_ls = 2;

  ExpectTrue("line may bleed two pixels below compact threshold",
             text_render_layout_utils::CurrentLineFitsScreen(
                 306, line_h, line_ls, max_height, bottom_margin));
  ExpectTrue("line beyond compact bleed does not fit",
             !text_render_layout_utils::CurrentLineFitsScreen(
                 307, line_h, line_ls, max_height, bottom_margin));
  ExpectFalse("line at compact bleed has no following line room",
              text_render_layout_utils::HasRoomForFollowingLine(
                  306, line_h, line_ls, max_height, bottom_margin));
  ExpectTrue("far below screen does not fit",
             !text_render_layout_utils::CurrentLineFitsScreen(
                 321, line_h, line_ls, max_height, bottom_margin));
  ExpectTrue("full-height HUD margin remains strict",
             !text_render_layout_utils::CurrentLineFitsScreen(
                 365, line_h, line_ls, 400, 36));
}

// Regression test for Bug 1 (descender clipping near HUD).
//
// The renderer keeps ts->margin.bottom at the unguarded value so the pixel clip
// fires at (max_height - unguarded_margin).  WouldOverflowReadingScreen is
// called with the guarded margin so overflow fires earlier — this gap is the
// safety zone in which misaligned descenders still remain visible.
//
// Invariant: any glyph that the overflow check admits (last non-overflowing
// pen_y) must have its maximum reasonable descent strictly below the unguarded
// pixel clip boundary.
void TestBottomSafeAreaRendererClipAboveOverflowThreshold() {
  const int max_height  = 400;
  const int unguarded   = 36;   // MARGINBOTTOM — used for pixel clip
  const int guard       = text_render_layout_utils::kFullReadingScreenFooterGuardPx; // 8
  const int guarded     = unguarded + guard;  // 44 — used for overflow check

  const int clip_boundary       = max_height - unguarded;  // 364
  const int overflow_threshold  = max_height - guarded;    // 356

  // The renderer clip must be above the overflow threshold.
  ExpectTrue("clip boundary is above overflow threshold",
             clip_boundary > overflow_threshold);
  // The gap must equal the guard constant.
  ExpectEq("gap between clip and threshold equals guard",
           clip_boundary - overflow_threshold, guard);

  // The last pen_y that does NOT trigger overflow (strict >):
  // WouldOverflow(pen_y, h=14, ls=1, 400, 44): pen_y+15 > 356 → pen_y > 341
  const int line_h  = 14;
  const int line_ls = 1;
  ExpectFalse("pen_y=341 with guarded margin is not overflow",
              text_render_layout_utils::WouldOverflowReadingScreen(
                  341, line_h, line_ls, max_height, guarded));
  ExpectTrue("pen_y=342 with guarded margin is overflow",
             text_render_layout_utils::WouldOverflowReadingScreen(
                 342, line_h, line_ls, max_height, guarded));

  // A glyph placed at pen_y=341 (last admitted line at 14px) with a generous
  // descent of 7px has its bottom pixel at sy=348, well below clip_boundary=364.
  const int last_admitted_pen_y    = overflow_threshold - line_h - line_ls; // 341
  const int conservative_descent   = guard - 1;  // 7 px
  ExpectTrue("descent at last admitted line fits within unguarded clip boundary",
             last_admitted_pen_y + conservative_descent < clip_boundary);

  // Even the edge case where PrintNewLine advances pen_y to exactly
  // overflow_threshold (= 356, since 341+15=356 and strict > does not fire)
  // must keep descenders below the unguarded clip boundary.
  const int edge_pen_y = overflow_threshold;  // 356
  ExpectTrue("descent at edge pen_y fits within unguarded clip boundary",
             edge_pen_y + conservative_descent < clip_boundary);
}

} // namespace

int main() {
  TestComputeRtlLineStartX();
  TestShouldAutoWrapGlyph();
  TestResolveClipRight();
  TestResolveReadingScreenMetrics();
  TestResolveReadingScreenMetricsForReadingScreen();
  TestResolveCompactReadingBottomMargin();
  TestWouldOverflowReadingScreen();
  TestWouldCurrentLineOverflowReadingScreen();
  TestCurrentLineVisibilityIgnoresFollowingLine();
  TestParagraphStartGuardAllowsOneLineParagraphInLastSlot();
  TestCurrentLineFitAllowsVisualLastLineWithoutFollowingRoom();
  TestBottomSafeAreaRendererClipAboveOverflowThreshold();
  return 0;
}

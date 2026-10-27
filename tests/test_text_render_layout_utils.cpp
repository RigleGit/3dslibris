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
           24);

  metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          true, 0, 36, 16);
  ExpectEq("turned-right first screen height", metrics.max_height, 320);
  ExpectEq("turned-right first screen bottom margin", metrics.bottom_margin,
           24);

  metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          true, 1, 36, 16);
  ExpectEq("turned-right second screen height", metrics.max_height, 400);
  ExpectEq("turned-right second screen bottom margin", metrics.bottom_margin,
           44);
}

void TestResolveReadingScreenMetricsForOrientation() {
  using text_render_layout_utils::ResolveReadingScreenMetricsForOrientation;
  using text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen;

  // Portrait orientations must match the boolean variant exactly.
  for (int orientation = 0; orientation <= 1; ++orientation) {
    for (int screen = 0; screen <= 1; ++screen) {
      const text_render_layout_utils::ReadingScreenMetrics expected =
          ResolveReadingScreenMetricsForReadingScreen(orientation == 1, screen,
                                                      36, 16);
      const text_render_layout_utils::ReadingScreenMetrics actual =
          ResolveReadingScreenMetricsForOrientation((unsigned char)orientation,
                                                    screen, 36, 16);
      ExpectEq("portrait orientation max_height parity", actual.max_height,
               expected.max_height);
      ExpectEq("portrait orientation bottom_margin parity",
               actual.bottom_margin, expected.bottom_margin);
    }
  }

  // Landscape: both screens are 240 px tall. The top has no HUD, so it only
  // needs the pagination safety guard. The bottom reserves HUD + guard.
  const text_render_layout_utils::ReadingScreenMetrics top =
      ResolveReadingScreenMetricsForOrientation(2, 0, 36, 16);
  ExpectEq("landscape top max_height", top.max_height, 240);
  ExpectEq("landscape top bottom_margin", top.bottom_margin,
           text_render_layout_utils::kFullReadingScreenFooterGuardPx);

  const text_render_layout_utils::ReadingScreenMetrics bottom =
      ResolveReadingScreenMetricsForOrientation(2, 1, 36, 16);
  ExpectEq("landscape bottom max_height", bottom.max_height, 240);
  ExpectEq("landscape bottom bottom_margin", bottom.bottom_margin,
           text_render_layout_utils::kLandscapeHudStripPx +
               text_render_layout_utils::kFullReadingScreenFooterGuardPx);
}

void TestResolveReadingScreenRenderBottomMarginForOrientation() {
  using text_render_layout_utils::ResolveReadingScreenRenderBottomMarginForOrientation;

  ExpectEq("turned-left first render margin",
           ResolveReadingScreenRenderBottomMarginForOrientation(0, 0, 36, 16),
           36);
  ExpectEq("turned-left second render margin",
           ResolveReadingScreenRenderBottomMarginForOrientation(0, 1, 36, 16),
           16);
  ExpectEq("turned-right first render margin",
           ResolveReadingScreenRenderBottomMarginForOrientation(1, 0, 36, 16),
           16);
  ExpectEq("turned-right second render margin",
           ResolveReadingScreenRenderBottomMarginForOrientation(1, 1, 36, 16),
           36);
  ExpectEq("landscape top render uses full physical height",
           ResolveReadingScreenRenderBottomMarginForOrientation(2, 0, 36, 16),
           0);
  ExpectEq("landscape bottom render margin stops at HUD",
           ResolveReadingScreenRenderBottomMarginForOrientation(2, 1, 36, 16),
           text_render_layout_utils::kLandscapeHudStripPx);
}

void TestLandscapeTopRendererKeepsPaginationGuardOutsideClip() {
  const int line_height = 12;
  const int line_spacing = 0;
  const int pen_y = 220;
  const text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForOrientation(
          2, 0, 36, 20);
  const int render_margin = text_render_layout_utils::
      ResolveReadingScreenRenderBottomMarginForOrientation(2, 0, 36, 20);
  const int next_y = pen_y + line_height + line_spacing;

  ExpectTrue("landscape paginator admits final guarded top-screen line",
             text_render_layout_utils::HasRoomForFollowingLine(
                 pen_y, line_height, line_spacing, metrics.max_height,
                 metrics.bottom_margin));
  ExpectTrue("landscape renderer admits the same top-screen line",
             text_render_layout_utils::CurrentLineFitsScreen(
                 next_y, line_height, line_spacing, metrics.max_height,
                 render_margin));
  ExpectEq("landscape top clip leaves the full guard after final baseline",
           metrics.max_height - render_margin - next_y,
           text_render_layout_utils::kFullReadingScreenFooterGuardPx);
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

void TestCurrentLineVisibilityKeepsFullScreenPaginationGuard() {
  const int max_height = 400;
  const int unguarded_bottom_margin = 36;
  const int guarded_bottom_margin =
      unguarded_bottom_margin +
      text_render_layout_utils::kFullReadingScreenFooterGuardPx;

  ExpectTrue("full-screen current line cannot use guarded safety band",
              text_render_layout_utils::CurrentLineBeyondReadingScreen(
                  360, max_height, guarded_bottom_margin));
  ExpectTrue("full-screen current line cannot sit at unguarded clip edge",
              text_render_layout_utils::CurrentLineBeyondReadingScreen(
                  364, max_height, guarded_bottom_margin));
  ExpectTrue("full-screen current line past unguarded clip still overflows",
             text_render_layout_utils::CurrentLineBeyondReadingScreen(
                 365, max_height, guarded_bottom_margin));
}

void TestCurrentLineFitsKeepsFullScreenPaginationGuard() {
  const int max_height = 400;
  const int guarded_bottom_margin =
      36 + text_render_layout_utils::kFullReadingScreenFooterGuardPx;
  const int line_h = 13;
  const int line_ls = 0;

  ExpectFalse("full-screen baseline inside guard does not fit",
             text_render_layout_utils::CurrentLineFitsScreen(
                 358, line_h, line_ls, max_height, guarded_bottom_margin));
  ExpectFalse("full-screen baseline at unguarded clip edge does not fit",
             text_render_layout_utils::CurrentLineFitsScreen(
                 364, line_h, line_ls, max_height, guarded_bottom_margin));
  ExpectFalse("full-screen baseline past unguarded clip edge overflows",
              text_render_layout_utils::CurrentLineFitsScreen(
                  365, line_h, line_ls, max_height, guarded_bottom_margin));
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

  ExpectFalse("line may not bleed below compact threshold",
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

void TestBandImageAdvanceKeepsLastVisibleBaseline() {
  const int max_height = 320;
  const int bottom_margin = 16;
  const int threshold = max_height - bottom_margin;

  ExpectFalse("band image may end on the last visible baseline",
              text_render_layout_utils::ShouldAdvanceAfterBandImage(
                  threshold, max_height, bottom_margin));
  ExpectTrue("band image advances once the baseline is below the screen",
             text_render_layout_utils::ShouldAdvanceAfterBandImage(
                 threshold + 1, max_height, bottom_margin));
}

void TestWouldOverflowAtGuardedFooterBoundary() {
  ExpectFalse("pen_y=341 with guarded margin is not overflow",
              text_render_layout_utils::WouldOverflowReadingScreen(
                  341, 14, 1, 400, 44));
  ExpectTrue("pen_y=342 with guarded margin is overflow",
             text_render_layout_utils::WouldOverflowReadingScreen(
                 342, 14, 1, 400, 44));
}

void TestPaginationGuardScalesForLargeFonts() {
  ExpectEq("small fonts keep legacy guard",
           text_render_layout_utils::ResolvePaginationSafetyGuard(12),
           text_render_layout_utils::kFullReadingScreenFooterGuardPx);
  ExpectTrue("large fonts reserve descender-safe guard",
             text_render_layout_utils::ResolvePaginationSafetyGuard(24) >
                 text_render_layout_utils::kFullReadingScreenFooterGuardPx);
}

} // namespace

int main() {
  TestComputeRtlLineStartX();
  TestShouldAutoWrapGlyph();
  TestResolveClipRight();
  TestResolveReadingScreenMetricsForReadingScreen();
  TestResolveReadingScreenMetricsForOrientation();
  TestResolveReadingScreenRenderBottomMarginForOrientation();
  TestLandscapeTopRendererKeepsPaginationGuardOutsideClip();
  TestResolveCompactReadingBottomMargin();
  TestWouldOverflowReadingScreen();
  TestWouldCurrentLineOverflowReadingScreen();
  TestCurrentLineVisibilityIgnoresFollowingLine();
  TestCurrentLineVisibilityKeepsFullScreenPaginationGuard();
  TestCurrentLineFitsKeepsFullScreenPaginationGuard();
  TestParagraphStartGuardAllowsOneLineParagraphInLastSlot();
  TestCurrentLineFitAllowsVisualLastLineWithoutFollowingRoom();
  TestBandImageAdvanceKeepsLastVisibleBaseline();
  TestWouldOverflowAtGuardedFooterBoundary();
  TestPaginationGuardScalesForLargeFonts();
  return 0;
}

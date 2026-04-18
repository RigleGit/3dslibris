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
  ExpectEq("caps compact footer to 16",
           text_render_layout_utils::ResolveCompactReadingBottomMargin(36), 16);
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

} // namespace

int main() {
  TestComputeRtlLineStartX();
  TestShouldAutoWrapGlyph();
  TestResolveClipRight();
  TestResolveReadingScreenMetrics();
  TestResolveReadingScreenMetricsForReadingScreen();
  TestResolveCompactReadingBottomMargin();
  TestWouldOverflowReadingScreen();
  return 0;
}

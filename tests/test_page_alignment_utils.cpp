#include "book/page_alignment_utils.h"

#include "shared/text_token_constants.h"
#include "test_assert.h"

namespace {

enum TextStyleId {
  kTextStyleBold = 1,
  kTextStyleBoldItalic = 3,
  kTextStyleMonoBold = 6,
  kTextStyleMonoBoldItalic = 8,
};

int MeasureGlyph(uint32_t, unsigned char style, void *) {
  switch (style) {
  case kTextStyleBold:
  case kTextStyleBoldItalic:
  case kTextStyleMonoBold:
  case kTextStyleMonoBoldItalic:
    return 2;
  default:
    return 1;
  }
}

void TestIgnoresControlTokensAndPayloads() {
  const uint32_t buf[] = {TEXT_LINK_START, 99, 'A', TEXT_LINK_END, 'B', '\n'};
  test::ExpectEq("link markers ignored",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 2);
}

void TestTracksStyleChangesWhileMeasuring() {
  const uint32_t buf[] = {'A', TEXT_BOLD_ON, 'B', TEXT_BOLD_OFF, 'C', '\n'};
  test::ExpectEq("style markers affect width",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 4);
}

void TestStopsAtBlockTokens() {
  const uint32_t buf[] = {'A', 'B', TEXT_IMAGE, 7, 'C', '\n'};
  test::ExpectEq("image token ends line measurement",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 2);
}

void TestIgnoresFontSizeTokenAndPayload() {
  const uint32_t buf[] = {'A', TEXT_FONT_SIZE, 24, ' ', 'B', '\n'};
  test::ExpectEq("font-size token ignored",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 3);
}

void TestIgnoresImageAlignTokenAndPayload() {
  const uint32_t buf[] = {'A', TEXT_IMAGE_ALIGN, 2, 'B', '\n'};
  test::ExpectEq("image-align token ignored",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 2);
}

void TestIgnoresLineStartXTokenAndPayload() {
  const uint32_t buf[] = {TEXT_LINE_START_X, 48, 'A', 'B', '\n'};
  test::ExpectEq("line-start-x token ignored",
                 page_alignment_utils::MeasureAlignedLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     MeasureGlyph, NULL),
                 2);
}

void TestVisualLineWidthFitsCompletely() {
  // "AB" = 2px; available = 10; no clamping needed
  const uint32_t buf[] = {'A', 'B', '\n'};
  test::ExpectEq("short line returned unchanged",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     10, MeasureGlyph, NULL),
                 2);
}

void TestVisualLineWidthBreaksAtWordBoundary() {
  // "AAA BBB" where each char = 1px, available = 5
  // "AAA " = 4px fits; adding 'B' (5th char of "BBB") -> "AAA B" = 5px fits;
  // adding second 'B' = 6px > 5 -> break before "BBB" at last space boundary
  // last_word_end_width recorded when space seen = 3 (width of "AAA")
  const uint32_t buf[] = {'A','A','A',' ','B','B','B','\n'};
  test::ExpectEq("breaks at word boundary on overflow",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     5, MeasureGlyph, NULL),
                 3);
}

void TestVisualLineWidthNoWordBoundaryHardBreak() {
  // Single long word "AAAA" = 4px, available = 3; no space → hard break
  const uint32_t buf[] = {'A','A','A','A','\n'};
  test::ExpectEq("hard break when no word boundary",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     3, MeasureGlyph, NULL),
                 3);
}

void TestVisualLineWidthSpaceAtOverflowEdge() {
  // "AA " = 3px, available = 3; space is the overflowing char itself
  const uint32_t buf[] = {'A','A',' ','B','\n'};
  test::ExpectEq("trailing space at overflow returns pre-space width",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     3, MeasureGlyph, NULL),
                 2);
}

void TestVisualLineWidthTracksStyleChanges() {
  // 'A'=1px, BOLD_ON, 'B'=2px, BOLD_OFF, ' '=1px, 'C'=1px; available=4
  // "AB " = 1+2+1=4px; 'C' would be 5 -> break before 'C' at space boundary
  // last_word_end_width = 3 ("AB" before space)
  const uint32_t buf[] = {'A', TEXT_BOLD_ON, 'B', TEXT_BOLD_OFF, ' ', 'C', '\n'};
  test::ExpectEq("style changes tracked in visual line measurement",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     4, MeasureGlyph, NULL),
                 3);
}

void TestVisualLineWidthSkipsControlTokens() {
  // TEXT_PARAGRAPH_CENTER then "AA BB"; center token must not affect width
  const uint32_t buf[] = {TEXT_PARAGRAPH_CENTER, 'A','A',' ','B','B','\n'};
  test::ExpectEq("paragraph-align token skipped in visual line measurement",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     3, MeasureGlyph, NULL),
                 2);
}

void TestVisualLineWidthMultipleWordBoundaries() {
  // "A B C D" each 1px, spaces 1px; available = 5
  // "A " = 2, "A B " = 4, "A B C" = 5, "A B C " = 6 > 5
  // last_word_end_width at second space = 3 ("A B" before second space)
  // then 'C' fits (4), third space overflows (5 exact = fits), 'D' overflows at 6
  // Actually let's trace carefully:
  // scan 'A'=1: line=1, no overflow
  // scan ' '=1: last_word_end=1, line=2
  // scan 'B'=1: line=3, no overflow
  // scan ' '=1: last_word_end=3, line=4
  // scan 'C'=1: line=5, no overflow (5 <= 5)
  // scan ' '=1: last_word_end=5, line=6 > 5 → overflow at space → return 5
  const uint32_t buf[] = {'A',' ','B',' ','C',' ','D','\n'};
  test::ExpectEq("last word boundary before overflow used",
                 page_alignment_utils::MeasureFirstVisualLineWidth(
                     buf, sizeof(buf) / sizeof(buf[0]), 0, false, false, false,
                     5, MeasureGlyph, NULL),
                 5);
}

} // namespace

int main() {
  TestIgnoresControlTokensAndPayloads();
  TestTracksStyleChangesWhileMeasuring();
  TestStopsAtBlockTokens();
  TestIgnoresFontSizeTokenAndPayload();
  TestIgnoresImageAlignTokenAndPayload();
  TestIgnoresLineStartXTokenAndPayload();
  TestVisualLineWidthFitsCompletely();
  TestVisualLineWidthBreaksAtWordBoundary();
  TestVisualLineWidthNoWordBoundaryHardBreak();
  TestVisualLineWidthSpaceAtOverflowEdge();
  TestVisualLineWidthTracksStyleChanges();
  TestVisualLineWidthSkipsControlTokens();
  TestVisualLineWidthMultipleWordBoundaries();
  return 0;
}

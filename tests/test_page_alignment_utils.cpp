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

} // namespace

int main() {
  TestIgnoresControlTokensAndPayloads();
  TestTracksStyleChangesWhileMeasuring();
  TestStopsAtBlockTokens();
  TestIgnoresFontSizeTokenAndPayload();
  TestIgnoresImageAlignTokenAndPayload();
  return 0;
}

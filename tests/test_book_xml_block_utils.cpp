#include "book/book_xml_block_utils.h"
#include "test_assert.h"

namespace {

void TestRecognizesEasyBlockTags() {
  test::ExpectTrue("aside is block",
                   book_xml_block_utils::IsEasyBlockTag(TAG_ASIDE));
  test::ExpectTrue("blockquote is block",
                   book_xml_block_utils::IsEasyBlockTag(TAG_BLOCKQUOTE));
  test::ExpectTrue("caption is block",
                   book_xml_block_utils::IsEasyBlockTag(TAG_CAPTION));
  test::ExpectTrue("dd is block",
                   book_xml_block_utils::IsEasyBlockTag(TAG_DD));
  test::ExpectTrue("figure is block",
                   book_xml_block_utils::IsEasyBlockTag(TAG_FIGURE));
  test::ExpectFalse("paragraph is not easy block",
                    book_xml_block_utils::IsEasyBlockTag(TAG_P));
}

void TestBlockSpacingPlans() {
  test::ExpectEq("aside start lf",
                 book_xml_block_utils::GetBlockStartLinefeeds(TAG_ASIDE), 1);
  test::ExpectEq("aside end lf",
                 book_xml_block_utils::GetBlockEndLinefeeds(TAG_ASIDE), 2);
  test::ExpectEq("blockquote end lf",
                 book_xml_block_utils::GetBlockEndLinefeeds(TAG_BLOCKQUOTE), 1);
  test::ExpectEq("caption end lf",
                 book_xml_block_utils::GetBlockEndLinefeeds(TAG_CAPTION), 1);
  test::ExpectEq("dd leading spaces",
                 book_xml_block_utils::GetLeadingSpaceCount(TAG_DD), 2);
}

void TestInnerParagraphSpacingSuppression() {
  test::ExpectTrue("blockquote suppresses inner p spacing",
                   book_xml_block_utils::SuppressInnerParagraphSpacing(
                       TAG_BLOCKQUOTE));
  test::ExpectTrue("dd suppresses inner p spacing",
                   book_xml_block_utils::SuppressInnerParagraphSpacing(TAG_DD));
  test::ExpectFalse("aside keeps inner p spacing",
                    book_xml_block_utils::SuppressInnerParagraphSpacing(
                        TAG_ASIDE));
}

} // namespace

int main() {
  TestRecognizesEasyBlockTags();
  TestBlockSpacingPlans();
  TestInnerParagraphSpacingSuppression();
  return 0;
}

#include "book/book_xml_list_utils.h"
#include "parse.h"
#include "test_assert.h"

extern "C" {

XML_Size XMLCALL XML_GetCurrentLineNumber(XML_Parser) { return 0; }
XML_Size XMLCALL XML_GetCurrentColumnNumber(XML_Parser) { return 0; }
enum XML_Error XMLCALL XML_GetErrorCode(XML_Parser) { return XML_ERROR_NONE; }
const XML_LChar *XMLCALL XML_ErrorString(enum XML_Error) { return ""; }

}

namespace {

void TestActiveListContextPrefersNearestList() {
  parsedata_t p{};
  parse_init(&p);

  test::ExpectEq("no active list",
                 (int)book_xml_list_utils::GetActiveListContext(&p),
                 (int)TAG_NONE);

  parse_push(&p, TAG_OL);
  test::ExpectEq("ordered list active",
                 (int)book_xml_list_utils::GetActiveListContext(&p),
                 (int)TAG_OL);

  parse_push(&p, TAG_UL);
  test::ExpectEq("nested unordered list wins",
                 (int)book_xml_list_utils::GetActiveListContext(&p),
                 (int)TAG_UL);
}

void TestOrderedListOrdinalTracksNearestOrderedList() {
  parsedata_t p{};
  parse_init(&p);

  parse_push(&p, TAG_OL);
  test::ExpectEq("first top-level item",
                 (int)book_xml_list_utils::AdvanceOrderedListOrdinal(&p), 1);
  test::ExpectEq("second top-level item",
                 (int)book_xml_list_utils::AdvanceOrderedListOrdinal(&p), 2);

  parse_push(&p, TAG_UL);
  parse_push(&p, TAG_OL);
  test::ExpectEq("first nested ordered item",
                 (int)book_xml_list_utils::AdvanceOrderedListOrdinal(&p), 1);
  test::ExpectEq("second nested ordered item",
                 (int)book_xml_list_utils::AdvanceOrderedListOrdinal(&p), 2);

  parse_pop(&p); // nested ol
  parse_pop(&p); // ul
  test::ExpectEq("top-level ordered list resumes",
                 (int)book_xml_list_utils::AdvanceOrderedListOrdinal(&p), 3);
}

void TestOrderedListMarkerFormatting() {
  test::ExpectStrEq("first marker",
                    book_xml_list_utils::BuildOrderedListMarker(1).c_str(),
                    "1.");
  test::ExpectStrEq("double-digit marker",
                    book_xml_list_utils::BuildOrderedListMarker(12).c_str(),
                    "12.");
  test::ExpectStrEq(
      "lower alpha marker",
      book_xml_list_utils::BuildOrderedListMarker(
          2, book_xml_list_utils::ORDERED_LIST_LOWER_ALPHA)
          .c_str(),
      "b.");
  test::ExpectStrEq(
      "upper alpha marker",
      book_xml_list_utils::BuildOrderedListMarker(
          27, book_xml_list_utils::ORDERED_LIST_UPPER_ALPHA)
          .c_str(),
      "AA.");
  test::ExpectStrEq(
      "lower roman marker",
      book_xml_list_utils::BuildOrderedListMarker(
          4, book_xml_list_utils::ORDERED_LIST_LOWER_ROMAN)
          .c_str(),
      "iv.");
}

void TestOrderedListStyleFollowsDepthAndTypeAttr() {
  parsedata_t p{};
  parse_init(&p);

  parse_push(&p, TAG_OL);
  book_xml_list_utils::ConfigureElementListSemantics(&p, NULL);
  test::ExpectEq("top-level ordered list is decimal",
                 (int)book_xml_list_utils::GetActiveOrderedListStyle(&p),
                 (int)book_xml_list_utils::ORDERED_LIST_DECIMAL);

  parse_push(&p, TAG_OL);
  book_xml_list_utils::ConfigureElementListSemantics(&p, NULL);
  test::ExpectEq("second-level ordered list defaults to lower alpha",
                 (int)book_xml_list_utils::GetActiveOrderedListStyle(&p),
                 (int)book_xml_list_utils::ORDERED_LIST_LOWER_ALPHA);

  parse_push(&p, TAG_OL);
  book_xml_list_utils::ConfigureElementListSemantics(&p, NULL);
  test::ExpectEq("third-level ordered list defaults to lower roman",
                 (int)book_xml_list_utils::GetActiveOrderedListStyle(&p),
                 (int)book_xml_list_utils::ORDERED_LIST_LOWER_ROMAN);

  parse_pop(&p);
  parse_pop(&p);
  parse_pop(&p);

  const char *attrs[] = {"type", "A", NULL};
  parse_push(&p, TAG_OL);
  book_xml_list_utils::ConfigureElementListSemantics(&p, attrs);
  test::ExpectEq("explicit type attribute wins",
                 (int)book_xml_list_utils::GetActiveOrderedListStyle(&p),
                 (int)book_xml_list_utils::ORDERED_LIST_UPPER_ALPHA);

  parse_pop(&p);

  parse_push(&p, TAG_OL);
  book_xml_list_utils::ConfigureElementListSemantics(&p, NULL);
  const char *decimal_attrs[] = {"type", "1", NULL};
  parse_push(&p, TAG_OL);
  book_xml_list_utils::ConfigureElementListSemantics(&p, decimal_attrs);
  test::ExpectEq("explicit decimal type survives nesting",
                 (int)book_xml_list_utils::GetActiveOrderedListStyle(&p),
                 (int)book_xml_list_utils::ORDERED_LIST_DECIMAL);
}

void TestListMarkerSuppressionFollowsClassAndAncestors() {
  parsedata_t p{};
  parse_init(&p);

  const char *simplelist_attrs[] = {"class", "simplelist", NULL};
  parse_push(&p, TAG_UL);
  book_xml_list_utils::ConfigureElementListSemantics(&p, simplelist_attrs);
  test::ExpectTrue("simplelist suppresses markers",
                   book_xml_list_utils::HasSuppressedListMarkerContext(&p));

  parse_pop(&p);

  const char *index_attrs[] = {"class", "index", NULL};
  parse_push(&p, TAG_DIV);
  book_xml_list_utils::ConfigureElementListSemantics(&p, index_attrs);
  parse_push(&p, TAG_UL);
  book_xml_list_utils::ConfigureElementListSemantics(&p, NULL);
  test::ExpectTrue("index ancestor suppresses markers",
                   book_xml_list_utils::HasSuppressedListMarkerContext(&p));
}

void TestListMarkerSuppressionUsesCssClassMap() {
  parsedata_t p{};
  parse_init(&p);

  epub_css_class_map::CssClassMargins style{};
  style.hide_list_markers = true;
  p.css_class_map["ornamentless"] = style;

  const char *attrs[] = {"class", "ornamentless", NULL};
  parse_push(&p, TAG_UL);
  book_xml_list_utils::ConfigureElementListSemantics(&p, attrs);

  test::ExpectTrue("css class map suppresses markers",
                   book_xml_list_utils::HasSuppressedListMarkerContext(&p));
}

void TestParseListMarkerHiddenCssClassLooksUpMapAtLiOpen() {
  // Verifies that ParseListMarkerHiddenCssClass checks the CSS class map so
  // that selectors like "li.classname { list-style-type: none; }" suppress
  // list markers without any hardcoded class names.
  parsedata_t p{};
  parse_init(&p);

  epub_css_class_map::CssClassMargins style{};
  style.hide_list_markers = true;
  p.css_class_map["list-none"] = style;

  const char *attrs[] = {"class", "list-none", NULL};
  test::ExpectTrue("css class map hides markers via direct check",
                   book_xml_list_utils::ParseListMarkerHiddenCssClass(&p, attrs));

  const char *unknown_attrs[] = {"class", "ordinary", NULL};
  test::ExpectFalse("unknown class does not hide markers",
                    book_xml_list_utils::ParseListMarkerHiddenCssClass(
                        &p, unknown_attrs));

  test::ExpectFalse("null attrs does not crash",
                    book_xml_list_utils::ParseListMarkerHiddenCssClass(&p, NULL));
}

void TestPendingListItemContentTracksNearestItem() {
  parsedata_t p{};
  parse_init(&p);

  parse_push(&p, TAG_LI);
  book_xml_list_utils::MarkCurrentListItemPending(&p, true);
  test::ExpectTrue("inside list item", book_xml_list_utils::IsInsideListItem(&p));
  test::ExpectTrue("pending list item content",
                   book_xml_list_utils::HasPendingListItemContent(&p));

  parse_push(&p, TAG_LI);
  book_xml_list_utils::MarkCurrentListItemPending(&p, true);
  book_xml_list_utils::ConsumePendingListItemContent(&p);
  test::ExpectFalse("nearest item pending consumed",
                    book_xml_list_utils::HasPendingListItemContent(&p));

  parse_pop(&p);
  test::ExpectTrue("outer item still pending",
                   book_xml_list_utils::HasPendingListItemContent(&p));

  book_xml_list_utils::ConsumePendingListItemContent(&p);
  test::ExpectFalse("pending consumed",
                    book_xml_list_utils::HasPendingListItemContent(&p));
}

void TestNestedListIndentOnlyAddsForNestedItems() {
  test::ExpectEq("no list has no indent",
                 book_xml_list_utils::ResolveNestedListItemIndentPx(0, 4), 0);
  test::ExpectEq("top-level list has no extra indent",
                 book_xml_list_utils::ResolveNestedListItemIndentPx(1, 4), 0);
  test::ExpectEq("nested list gets visible indent",
                 book_xml_list_utils::ResolveNestedListItemIndentPx(2, 4), 12);
  test::ExpectEq("indent has minimum for narrow fonts",
                 book_xml_list_utils::ResolveNestedListItemIndentPx(2, 1), 12);
}

} // namespace

int main() {
  TestActiveListContextPrefersNearestList();
  TestOrderedListOrdinalTracksNearestOrderedList();
  TestOrderedListMarkerFormatting();
  TestOrderedListStyleFollowsDepthAndTypeAttr();
  TestListMarkerSuppressionFollowsClassAndAncestors();
  TestListMarkerSuppressionUsesCssClassMap();
  TestParseListMarkerHiddenCssClassLooksUpMapAtLiOpen();
  TestPendingListItemContentTracksNearestItem();
  TestNestedListIndentOnlyAddsForNestedItems();
  return 0;
}

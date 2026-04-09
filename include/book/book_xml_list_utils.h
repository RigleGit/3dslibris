#pragma once

#include <string>

#include "parse.h"

namespace book_xml_list_utils {

enum ordered_list_style_t : u8 {
  ORDERED_LIST_DECIMAL = 0,
  ORDERED_LIST_LOWER_ALPHA = 1,
  ORDERED_LIST_UPPER_ALPHA = 2,
  ORDERED_LIST_LOWER_ROMAN = 3,
  ORDERED_LIST_UPPER_ROMAN = 4,
};

void ConfigureElementListSemantics(parsedata_t *p, const char **attr);
context_t GetActiveListContext(const parsedata_t *p);
unsigned int GetActiveListDepth(const parsedata_t *p);
ordered_list_style_t GetActiveOrderedListStyle(const parsedata_t *p);
bool HasSuppressedListMarkerContext(const parsedata_t *p);
bool IsInsideListItem(const parsedata_t *p);
bool HasPendingListItemContent(const parsedata_t *p);
void MarkCurrentListItemPending(parsedata_t *p, bool pending);
void ConsumePendingListItemContent(parsedata_t *p);
unsigned int AdvanceOrderedListOrdinal(parsedata_t *p);
std::string BuildOrderedListMarker(unsigned int ordinal);
std::string BuildOrderedListMarker(unsigned int ordinal,
                                   ordered_list_style_t style);

} // namespace book_xml_list_utils

#pragma once

#include "parse.h"

namespace book_xml_block_utils {

bool IsEasyBlockTag(context_t tag);
int GetBlockStartLinefeeds(context_t tag);
int GetBlockEndLinefeeds(context_t tag);
int GetLeadingSpaceCount(context_t tag);
bool SuppressInnerParagraphSpacing(context_t tag);

} // namespace book_xml_block_utils

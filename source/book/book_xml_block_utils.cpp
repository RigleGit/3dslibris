#include "book/book_xml_block_utils.h"

namespace book_xml_block_utils {

bool IsEasyBlockTag(context_t tag) {
  return tag == TAG_ASIDE || tag == TAG_BLOCKQUOTE || tag == TAG_CAPTION ||
         tag == TAG_DD || tag == TAG_FIGURE;
}

int GetBlockStartLinefeeds(context_t tag) {
  if (!IsEasyBlockTag(tag))
    return 0;
  return 1;
}

int GetBlockEndLinefeeds(context_t tag) {
  switch (tag) {
  case TAG_ASIDE:
    return 2;
  case TAG_BLOCKQUOTE:
  case TAG_CAPTION:
  case TAG_DD:
  case TAG_FIGURE:
    return 1;
  default:
    return 0;
  }
}

int GetLeadingSpaceCount(context_t tag) {
  if (tag == TAG_DD)
    return 2;
  return 0;
}

bool SuppressInnerParagraphSpacing(context_t tag) {
  return tag == TAG_BLOCKQUOTE || tag == TAG_DD;
}

} // namespace book_xml_block_utils

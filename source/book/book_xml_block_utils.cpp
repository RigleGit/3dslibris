#include "book/book_xml_block_utils.h"

namespace book_xml_block_utils {

bool SuppressInnerParagraphSpacing(context_t tag) {
  return tag == TAG_BLOCKQUOTE || tag == TAG_DD;
}

} // namespace book_xml_block_utils

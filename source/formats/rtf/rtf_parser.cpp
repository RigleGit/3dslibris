#include "formats/rtf/rtf_parser.h"

#include "formats/common/book_error.h"
#include "formats/common/file_read_utils.h"
#include "formats/common/plain_parser.h"
#include "formats/rtf/rtf_loader.h"
#include "book/book.h"
#include "shared/debug_log.h"

#include <string.h>
#include <string>

namespace rtf_parser {

uint8_t Parse(Book *book, const char *path) {
  std::string text;
  if (!rtf_loader::ReadAndDecode(path, &text)) {
#ifdef DSLIBRIS_DEBUG
    int err = file_read_utils::LastErrorNumber();
    DBG_LOGF(book ? book->GetStatusReporter() : NULL,
             "RTF read failed path=%s op=%s errno=%d strerror=%s",
             path ? path : "(null)", file_read_utils::LastErrorOperation(),
             err, err ? strerror(err) : "none");
#endif
    return BOOK_ERR_CORRUPT;
  }
  return plain_parser::ParseBuffer(book, text);
}

} // namespace rtf_parser

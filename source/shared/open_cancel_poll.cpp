#include "shared/open_cancel_poll.h"

#include "book/book.h"
#include "shared/status_reporter.h"

namespace open_cancel_poll {

void Reset() {}

bool Poll(Book *book, IStatusReporter *reporter, const char *stage) {
  (void)stage;
  if (!book)
    return reporter && reporter->ShouldAbortWork();
  if ((reporter && reporter->ShouldAbortWork()) || book->IsOpenAbortRequested())
    return true;

  // This helper may run inside worker threads. Keep it side-effect free and
  // stateless so callers can poll aggressively without touching libctru state
  // or sharing mutable globals across threads.

  return (reporter && reporter->ShouldAbortWork()) ||
         book->IsOpenAbortRequested();
}

} // namespace open_cancel_poll

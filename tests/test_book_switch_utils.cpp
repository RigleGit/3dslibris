#include "reader/book_switch_utils.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

// Collaborators only store state and observe Close. The real transition must
// detach the current book before invoking external cleanup.
struct App;
struct Book {
  App *owner=nullptr; bool require_detached=false; int closes=0;
  void Close();
};
struct App {
  Book *current=nullptr; unsigned int session=7; unsigned int ready_at=123;
  Book *GetCurrentBook() const { return current; }
  void SetCurrentBook(Book *value) { current=value; }
  void SetCurrentBookSessionId(unsigned int value) { session=value; }
  void SetPdfDeferredReadyAtMs(unsigned int value) { ready_at=value; }
};
void Book::Close() {
  if (require_detached) {
    assert(owner && owner->current == nullptr);
    assert(owner->session == 0 && owner->ready_at == 0);
  }
  ++closes;
}
#define DBG_LOGF(...) ((void)0)
namespace reader_internal {
#include "book_switch_under_test.inc"
}
static void TestSwitchAndFailedOpenCleanup() {
  using namespace reader_internal;
  App app; Book current, next; current.owner=&app; current.require_detached=true;
  DetachCurrentBookForSwitch(nullptr, &next, 8, "test");
  DetachCurrentBookForSwitch(&app, &next, 8, "test");
  assert(current.closes == 0 && next.closes == 0);
  app.current=&current;
  DetachCurrentBookForSwitch(&app, &current, 8, "test");
  assert(current.closes == 0 && app.current == &current && app.session == 7 && app.ready_at == 123);
  DetachCurrentBookForSwitch(&app, &next, 8, "test");
  assert(current.closes == 1 && next.closes == 0 && app.current == nullptr);
  app.current=&current; app.session=9; app.ready_at=456;
  DetachCurrentBookForSwitch(&app, nullptr, 0, "leave");
  assert(current.closes == 2 && app.current == nullptr);
  CloseFailedOpenBook(&app, nullptr, 8, "missing");
  CloseFailedOpenBook(&app, &next, 8, "aborted");
  assert(next.closes == 1 && current.closes == 2);
}

namespace {

[[noreturn]] void Fail(const std::string &msg) {
  fprintf(stderr, "%s\n", msg.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void ExpectCString(const char *label, const char *actual, const char *expected) {
  if (std::string(actual ? actual : "") != std::string(expected)) {
    Fail(std::string(label) + ": expected " + expected + ", got " +
         (actual ? actual : "(null)"));
  }
}


void TestShouldAttachOpeningResult() {
  ExpectTrue("valid attach", ShouldAttachOpeningResult(7, 7, false, 4));
  ExpectFalse("stale attach blocked",
              ShouldAttachOpeningResult(0, 7, false, 4));
  ExpectFalse("session mismatch blocked",
              ShouldAttachOpeningResult(7, 8, false, 4));
  ExpectFalse("aborted blocked",
              ShouldAttachOpeningResult(7, 7, true, 4));
  ExpectFalse("empty pages blocked",
              ShouldAttachOpeningResult(7, 7, false, 0));
}

void TestDescribeOpeningFailureCause() {
  ExpectCString("stale cause",
                DescribeOpeningFailureCause(0, 7, false, 4),
                "stale-session");
  ExpectCString("session cause",
                DescribeOpeningFailureCause(7, 8, false, 4),
                "session-mismatch");
  ExpectCString("aborted cause",
                DescribeOpeningFailureCause(7, 7, true, 4),
                "aborted");
  ExpectCString("empty cause",
                DescribeOpeningFailureCause(7, 7, false, 0),
                "empty-parse");
}

} // namespace

int main() {
  TestSwitchAndFailedOpenCleanup();
  TestShouldAttachOpeningResult();
  TestDescribeOpeningFailureCause();
  return 0;
}

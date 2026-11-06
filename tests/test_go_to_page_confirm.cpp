#include <cassert>
#include <memory>
#include "settings/go_to_page_slider_utils.h"
static unsigned long long osGetTime() { return 1000; }
struct Book {
  int position = 5, target = -1, advances = 0;
  bool fail = false;
  int GetPageCount() const { return 80; }
  bool IsFixedLayout() const { return false; }
  void SetPosition(int page) { target = page; }
  bool HasPendingReflowNavigation() const { return target >= 0; }
  int AdvanceReflowPageCache() {
    if (fail) return -1;
    if (++advances == 1) return 1;
    position = target; target = -1; return 0;
  }
};
struct Text {};
struct App {
  Book book;
  bool relayout = true, abort_work = false, returned = false;
  int captured_position = -1;
  std::unique_ptr<Text> ts;
  Book *GetCurrentBook() { return &book; }
  bool BookNeedsRelayout(Book *) { return relayout; }
  bool ShouldAbortWork() { return abort_work; }
  void PrintStatus(const char *) {}
  void ReturnFromPrefs() { returned = true; captured_position = book.position; }
  void ShowCurrentBookView() { assert(false); }
  void SetPdfDeferredReadyAtMs(unsigned long long) { assert(false); }
  void RequestStatusRedraw() { assert(false); }
};
namespace book_renderer {
void CancelFixedLayoutDeferredWork(Book *) { assert(false); }
void ResetFixedLayoutViewportForNavigation(Book *) { assert(false); }
void DrawCurrentView(Book *, Text *) { assert(false); }
bool HasPendingFixedLayoutDeferredWork(Book *) { assert(false); return false; }
unsigned GetFixedLayoutDeferredDelayMs(Book *) { assert(false); return 0; }
}
class GoToPageDialog {
public:
  App &app_;
  bool open_ = true;
  int target_page_ = 70;
  explicit GoToPageDialog(App &app) : app_(app) {}
  bool Confirm();
};
#include "go_to_confirm_owner.inc"
int main() {
  App success;
  GoToPageDialog dialog(success);
  assert(dialog.Confirm());
  assert(success.returned && success.captured_position == 70 && !dialog.open_);
  App failed;
  failed.book.fail = true;
  GoToPageDialog failed_dialog(failed);
  assert(!failed_dialog.Confirm() && !failed.returned && failed_dialog.open_);
  App suspended;
  suspended.abort_work = true;
  GoToPageDialog suspended_dialog(suspended);
  assert(!suspended_dialog.Confirm() && !suspended.returned && suspended_dialog.open_);
}

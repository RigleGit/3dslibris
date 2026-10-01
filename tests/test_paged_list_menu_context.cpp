#include "menus/paged_list_menu_context.h"
#include "test_assert.h"

namespace {

struct Recording {
  int book_calls = 0, current_calls = 0, settings_calls = 0;
  int status_calls = 0, context_calls = 0, touch_calls = 0;
  Book *book = nullptr;
  bool from_book = false, book_context = true;
  FrameInput input;
  touchPosition mapped = {12, 34};
};

Book *GetBook(void *userdata) {
  Recording &r = *static_cast<Recording *>(userdata);
  ++r.book_calls;
  return r.book;
}
void ShowCurrent(void *userdata) {
  ++static_cast<Recording *>(userdata)->current_calls;
}
void ShowSettings(void *userdata, bool from_book) {
  Recording &r = *static_cast<Recording *>(userdata);
  ++r.settings_calls;
  r.from_book = from_book;
}
void RequestStatus(void *userdata) {
  ++static_cast<Recording *>(userdata)->status_calls;
}
bool IsBookSettings(void *userdata) {
  Recording &r = *static_cast<Recording *>(userdata);
  ++r.context_calls;
  return r.book_context;
}
touchPosition MapTouch(void *userdata, const FrameInput &input) {
  Recording &r = *static_cast<Recording *>(userdata);
  ++r.touch_calls;
  r.input = input;
  return r.mapped;
}

} // namespace

int main() {
  Recording first;
  // This opaque Book handle is returned but never dereferenced by the context.
  int book_token = 0;
  first.book = reinterpret_cast<Book *>(&book_token);
  PagedListMenuContext context = {};
  context.userdata = &first;
  context.get_current_book = GetBook;
  context.show_current_book_view = ShowCurrent;
  context.show_settings_view = ShowSettings;
  context.request_status_redraw = RequestStatus;
  context.is_book_settings_context = IsBookSettings;
  context.map_touch = MapTouch;

  test::ExpectTrue("current book result", context.GetCurrentBook() == first.book);
  first.book = nullptr;
  test::ExpectTrue("closed book is observed on next request",
                   context.GetCurrentBook() == nullptr);
  test::ExpectEq("current book callback count", first.book_calls, 2);
  context.ShowCurrentBookView();
  test::ExpectEq("current view callback count", first.current_calls, 1);
  context.ShowSettingsView(true);
  test::ExpectTrue("book origin forwarded", first.from_book);
  context.ShowSettingsView(false);
  test::ExpectFalse("browser origin forwarded", first.from_book);
  test::ExpectEq("settings callback count", first.settings_calls, 2);
  context.RequestStatusRedraw();
  test::ExpectEq("status redraw callback count", first.status_calls, 1);
  test::ExpectTrue("book context result", context.IsBookSettingsContext());
  first.book_context = false;
  test::ExpectFalse("book context follows current state",
                    context.IsBookSettingsContext());
  test::ExpectEq("book context callback count", first.context_calls, 2);
  const FrameInput input(0x80000004u, 0x40000008u, true, 111, 67,
                         UINT64_C(0x200000005));
  const touchPosition pos = context.MapTouch(input);
  test::ExpectTrue("mapped touch result", pos.px == 12 && pos.py == 34);
  test::ExpectTrue("touch receives the captured frame",
                   first.input.keys_down == input.keys_down &&
                   first.input.keys_held == input.keys_held &&
                   first.input.touch_active && first.input.touch_raw_x == 111 &&
                   first.input.touch_raw_y == 67 &&
                   first.input.timestamp_ms == UINT64_C(0x200000005));
  test::ExpectEq("touch callback count", first.touch_calls, 1);

  Recording second;
  PagedListMenuContext partial = {};
  partial.userdata = &second;
  partial.request_status_redraw = RequestStatus;
  partial.RequestStatusRedraw();
  test::ExpectEq("partial context receives its own userdata", second.status_calls, 1);
  test::ExpectEq("first context is not called", first.status_calls, 1);
  partial.ShowCurrentBookView();
  partial.ShowSettingsView(true);
  test::ExpectTrue("missing book callback returns null",
                   partial.GetCurrentBook() == nullptr);
  test::ExpectFalse("missing context callback returns false",
                    partial.IsBookSettingsContext());
  const touchPosition empty_pos = partial.MapTouch(input);
  test::ExpectTrue("missing touch callback returns origin",
                   empty_pos.px == 0 && empty_pos.py == 0);
  partial.request_status_redraw = nullptr;
  partial.RequestStatusRedraw();
  test::ExpectEq("removed callback is not invoked", second.status_calls, 1);
  return 0;
}

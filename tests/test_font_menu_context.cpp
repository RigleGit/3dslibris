#include "settings/font_menu_context.h"
#include "test_assert.h"

#include <string>

namespace {

struct Recording {
  int settings_calls = 0, book_context_calls = 0, touch_calls = 0;
  int status_calls = 0, layout_calls = 0, refresh_calls = 0, write_calls = 0;
  bool from_book = false, book_context = true;
  FrameInput input;
  std::string status;
  int button_id = -1, write_result = 123;
  touchPosition mapped = {21, 43};
};

void ShowSettings(void *userdata, bool from_book) {
  Recording &r = *static_cast<Recording *>(userdata);
  ++r.settings_calls;
  r.from_book = from_book;
}
bool IsBookSettings(void *userdata) {
  Recording &r = *static_cast<Recording *>(userdata);
  ++r.book_context_calls;
  return r.book_context;
}
touchPosition MapTouch(void *userdata, const FrameInput &input) {
  Recording &r = *static_cast<Recording *>(userdata);
  ++r.touch_calls;
  r.input = input;
  return r.mapped;
}
void PrintStatus(void *userdata, const char *message) {
  Recording &r = *static_cast<Recording *>(userdata);
  ++r.status_calls;
  r.status = message;
}
void MarkLayout(void *userdata) {
  ++static_cast<Recording *>(userdata)->layout_calls;
}
void RefreshButton(void *userdata, int id) {
  Recording &r = *static_cast<Recording *>(userdata);
  ++r.refresh_calls;
  r.button_id = id;
}
int WritePrefs(void *userdata) {
  Recording &r = *static_cast<Recording *>(userdata);
  ++r.write_calls;
  return r.write_result;
}

} // namespace

int main() {
  Recording first;
  std::string font_dir = "/fonts";
  FontMenuContext context = {};
  context.userdata = &first;
  context.font_dir = &font_dir;
  context.show_settings_view = ShowSettings;
  context.is_book_settings_context = IsBookSettings;
  context.map_touch = MapTouch;
  context.print_status = PrintStatus;
  context.mark_book_layout_dirty = MarkLayout;
  context.refresh_prefs_button = RefreshButton;
  context.write_prefs = WritePrefs;

  test::ExpectTrue("font directory", context.FontDir() == "/fonts");
  font_dir = "/sd/fonts";
  test::ExpectTrue("font directory follows current configuration",
                   context.FontDir() == "/sd/fonts");
  context.ShowSettingsView(true);
  test::ExpectTrue("book origin forwarded", first.from_book);
  context.ShowSettingsView(false);
  test::ExpectFalse("browser origin forwarded", first.from_book);
  test::ExpectEq("one callback per settings request", first.settings_calls, 2);
  test::ExpectTrue("book context result", context.IsBookSettingsContext());
  first.book_context = false;
  test::ExpectFalse("book context result is not cached",
                    context.IsBookSettingsContext());
  test::ExpectEq("book context callback count", first.book_context_calls, 2);

  const FrameInput input(0x80000001u, 0x40000002u, true, 103, 57,
                         UINT64_C(0x100000007));
  const touchPosition pos = context.MapTouch(input);
  test::ExpectTrue("mapped result forwarded", pos.px == 21 && pos.py == 43);
  test::ExpectTrue("snapshot forwarded intact",
                   first.input.keys_down == input.keys_down &&
                   first.input.keys_held == input.keys_held &&
                   first.input.touch_active && first.input.touch_raw_x == 103 &&
                   first.input.touch_raw_y == 57 &&
                   first.input.timestamp_ms == UINT64_C(0x100000007));
  test::ExpectEq("touch callback count", first.touch_calls, 1);
  context.PrintStatus("font loaded");
  test::ExpectTrue("status contents forwarded", first.status == "font loaded");
  test::ExpectEq("status callback count", first.status_calls, 1);
  context.MarkBookLayoutDirty();
  test::ExpectEq("layout callback count", first.layout_calls, 1);
  context.RefreshPrefsButton(17);
  test::ExpectEq("refresh button argument", first.button_id, 17);
  test::ExpectEq("refresh callback count", first.refresh_calls, 1);
  test::ExpectEq("successful write result", context.WritePrefs(), 123);
  first.write_result = -7;
  test::ExpectEq("failed write result preserved", context.WritePrefs(), -7);
  test::ExpectEq("write callback count", first.write_calls, 2);

  Recording second;
  second.write_result = 456;
  // A context with just the persistence callback still supports its operation.
  FontMenuContext partial = {};
  partial.userdata = &second;
  partial.write_prefs = WritePrefs;
  partial.ShowSettingsView(false);
  partial.PrintStatus("ignored");
  partial.MarkBookLayoutDirty();
  partial.RefreshPrefsButton(9);
  test::ExpectFalse("missing book context callback",
                    partial.IsBookSettingsContext());
  const touchPosition empty_pos = partial.MapTouch(input);
  test::ExpectTrue("missing touch callback returns origin",
                   empty_pos.px == 0 && empty_pos.py == 0);
  test::ExpectTrue("missing directory returns empty", partial.FontDir().empty());
  test::ExpectEq("partial context receives its own userdata",
                 partial.WritePrefs(), 456);
  test::ExpectEq("partial write callback count", second.write_calls, 1);
  test::ExpectEq("other context remains untouched", first.write_calls, 2);
  partial.write_prefs = nullptr;
  test::ExpectEq("missing persistence callback reports failure",
                 partial.WritePrefs(), -1);
  test::ExpectEq("no write after callback removed", second.write_calls, 1);
  return 0;
}

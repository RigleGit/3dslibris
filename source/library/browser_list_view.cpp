#include "library/browser_list_view.h"

#include "app/app.h"
#include "book/book.h"
#include "library/browser_presentation_hit_utils.h"
#include "library/browser_presentation_utils.h"
#include "library/browser_view_utils.h"
#include "ui/text.h"

namespace browser_list_view {

int HitTestBookIndex(int x, int y, int page_start, int book_count,
                     int page_size) {
  return browser_presentation_hit_utils::HitTestListBookIndex(
      x, y, page_start, book_count, page_size, kRowX, kRowY0, kRowW, kRowH,
      kRowPitch);
}

void DrawPage(App &app, int page_start, int page_size) {
  for (int i = page_start;
       i < app.BookCount() && i < page_start + page_size; i++) {
    const int row = i - page_start;
    const int row_x = kRowX;
    const int row_y = kRowY0 + row * kRowPitch;
    const bool selected = app.books[i] == app.GetSelectedBook();
    const browser_view_utils::ListRowPalette palette =
        browser_view_utils::PaletteForListRow(selected, app.ts->GetColorMode());
    app.ts->FillRect((u16)row_x, (u16)row_y, (u16)(row_x + kRowW),
                     (u16)(row_y + kRowH), palette.fill);
    app.ts->DrawRect((u16)row_x, (u16)row_y, (u16)(row_x + kRowW),
                     (u16)(row_y + kRowH), palette.border);

    app.ts->SetPixelSize(10);
    app.ts->SetStyle(selected ? TEXT_STYLE_BOLD : TEXT_STYLE_REGULAR);
    app.ts->SetTextColorOverride(palette.text);
    std::string display_name =
        browser_presentation_utils::BuildBrowserDisplayName(app.books[i]);
    const int progress_x = row_x + kRowW - 44;
    const int title_width = progress_x - row_x - 6;
    const int title_height =
        browser_view_utils::ListTitleBoxHeight(app.ts->GetHeight());
    browser_presentation_utils::DrawWrappedTitleInsideCover(
        app.ts.get(), display_name, row_x, row_y, title_width, title_height,
        app.ts->GetStyle());

    int pos = app.books[i]->GetPosition();
    char msg[16];
    if (pos > 0)
      snprintf(msg, sizeof(msg), "Pg %d", pos + 1);
    else
      snprintf(msg, sizeof(msg), "NEW");
    app.ts->SetPen(progress_x, row_y + 14);
    app.ts->PrintString(msg);
    app.ts->ClearTextColorOverride();
  }
}

} // namespace browser_list_view

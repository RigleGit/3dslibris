#include "library/browser_list_view.h"

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

void DrawPage(const BrowserDrawContext &ctx, int page_start, int page_size) {
  const int book_count = (int)ctx.books->size();
  for (int i = page_start; i < book_count && i < page_start + page_size; i++) {
    const int row = i - page_start;
    const int row_x = kRowX;
    const int row_y = kRowY0 + row * kRowPitch;
    const bool selected = (*ctx.books)[i] == ctx.selected_book;
    const browser_view_utils::ListRowPalette palette =
        browser_view_utils::PaletteForListRow(selected, ctx.ts->GetColorMode());
    ctx.ts->FillRect((u16)row_x, (u16)row_y, (u16)(row_x + kRowW),
                     (u16)(row_y + kRowH), palette.fill);
    ctx.ts->DrawRect((u16)row_x, (u16)row_y, (u16)(row_x + kRowW),
                     (u16)(row_y + kRowH), palette.border);

    ctx.ts->SetPixelSize(10);
    ctx.ts->SetStyle(selected ? TEXT_STYLE_BOLD : TEXT_STYLE_REGULAR);
    ctx.ts->SetTextColorOverride(palette.text);
    std::string display_name =
        browser_presentation_utils::BuildBrowserDisplayName((*ctx.books)[i]);
    const int progress_x = row_x + kRowW - 44;
    const int title_width = progress_x - row_x - 6;
    const int title_height =
        browser_view_utils::ListTitleBoxHeight(ctx.ts->GetHeight());
    browser_presentation_utils::DrawWrappedTitleInsideCover(
        ctx.ts, display_name, row_x, row_y, title_width, title_height,
        ctx.ts->GetStyle());

    if (!(*ctx.books)[i]->IsBrowserFolder()) {
      int pos = (*ctx.books)[i]->GetPosition();
      char msg[16];
      if (pos > 0)
        snprintf(msg, sizeof(msg), "Pg %d", pos + 1);
      else
        snprintf(msg, sizeof(msg), "NEW");
      ctx.ts->SetPen(progress_x, row_y + 14);
      ctx.ts->PrintString(msg);
    }
    ctx.ts->ClearTextColorOverride();
  }
}

} // namespace browser_list_view

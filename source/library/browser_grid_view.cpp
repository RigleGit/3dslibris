#include "library/browser_grid_view.h"

#include <cstring>

#include "app/app.h"
#include "book/book.h"
#include "library/browser_presentation_hit_utils.h"
#include "library/browser_presentation_utils.h"
#include "ui/button.h"
#include "ui/text.h"

BrowserGridMarqueeState::BrowserGridMarqueeState()
    : book(NULL), strip(NULL), bg_strip(NULL), strip_w(0), strip_h(0),
      strip_x(0), strip_y(0), blit_w(0), scroll_offset(0), scroll_timer(0),
      end_timer(0), active(false) {}

void BrowserGridMarqueeState::Reset() {
  delete[] strip;
  delete[] bg_strip;
  strip = NULL;
  bg_strip = NULL;
  book = NULL;
  strip_w = strip_h = strip_x = strip_y = blit_w = 0;
  scroll_offset = scroll_timer = end_timer = 0;
  active = false;
}

namespace browser_grid_view {

int HitTestBookIndex(int x, int y, int page_start, int book_count) {
  return browser_presentation_hit_utils::HitTestGridBookIndex(
      x, y, page_start, book_count, kGridX0, kGridY0, kCellW, kCellH, kGridCols,
      kGridRows, APP_BROWSER_BUTTON_COUNT);
}

void DrawPage(App &app, BrowserGridMarqueeState &marquee, int page_start) {
  for (int i = page_start;
       i < app.BookCount() && i < page_start + APP_BROWSER_BUTTON_COUNT; i++) {
    app.buttons[i]->Draw(app.ts->screenright, app.books[i] == app.GetSelectedBook());

    int page_idx = i % APP_BROWSER_BUTTON_COUNT;
    int col = page_idx % kGridCols;
    int row = page_idx / kGridCols;
    int btnX = kGridX0 + col * kCellW;
    int btnY = kGridY0 + row * kCellH;

    if (app.books[i]->coverPixels) {
      int cx = btnX + 2 + (kCoverW - app.books[i]->coverWidth) / 2;
      int cy = btnY + 2 + (kCoverH - app.books[i]->coverHeight) / 2;
      int w = app.ts->display.height;
      app.ts->MarkScreenDirtyRect(app.ts->screenright, cx, cy,
                                  cx + app.books[i]->coverWidth,
                                  cy + app.books[i]->coverHeight);
      for (int py = 0; py < app.books[i]->coverHeight && (cy + py) < 320; py++) {
        for (int px = 0; px < app.books[i]->coverWidth && (cx + px) < 240; px++) {
          app.ts->screenright[(cy + py) * w + (cx + px)] =
              app.books[i]->coverPixels[py * app.books[i]->coverWidth + px];
        }
      }
    }

    if (app.books[i] == app.GetSelectedBook()) {
      app.ts->DrawRect(btnX - 2, btnY - 2, btnX + kCellW + 2, btnY + kCellH + 2,
                       0xF800);
      app.ts->DrawRect(btnX - 3, btnY - 3, btnX + kCellW + 3, btnY + kCellH + 3,
                       0xF800);
      app.ts->SetStyle(TEXT_STYLE_BOLD);
    } else {
      app.ts->SetStyle(TEXT_STYLE_REGULAR);
    }

    app.ts->SetPixelSize(10);
    std::string display_name =
        browser_presentation_utils::BuildBrowserDisplayName(app.books[i]);
    if (app.books[i]->coverPixels) {
      if (!display_name.empty()) {
        const char *dname = display_name.c_str();
        unsigned char cur_style = (unsigned char)app.ts->GetStyle();
        int full_w = (int)app.ts->GetStringWidth(dname, cur_style);
        Book *book_i = app.books[i];
        Book *sel = app.GetSelectedBook();
        bool overflows = full_w > kCellW;
        bool is_selected = book_i == sel;

        int saved_margin_right = app.ts->margin.right;
        bool saved_clip = app.ts->IsClipToContentEnabled();
        bool saved_wrap = app.ts->IsAutoWrapEnabled();

        app.ts->margin.right = app.ts->display.width - (btnX + kCellW);
        app.ts->SetClipToContentEnabled(true);
        app.ts->SetAutoWrapEnabled(false);

        if (is_selected && overflows) {
          const int glyph_top = btnY + kTitleOffsetY - app.ts->GetHeight();
          const int title_y = (glyph_top >= 0) ? glyph_top : 0;
          const int sh = app.ts->GetHeight() + 2;
          const int sw = full_w;
          const int fb_stride = app.ts->display.height;

          if (marquee.book != book_i) {
            const int vis_w = (btnX + kCellW <= app.ts->display.width)
                                  ? kCellW
                                  : app.ts->display.width - btnX;

            marquee.Reset();
            marquee.book = book_i;
            marquee.strip_w = sw;
            marquee.strip_h = sh;
            marquee.strip_x = btnX;
            marquee.strip_y = title_y;
            marquee.blit_w = vis_w;
            marquee.strip = new unsigned short[sw * sh];
            marquee.bg_strip = new unsigned short[vis_w * sh];

            for (int r = 0; r < sh; r++) {
              const unsigned short *src =
                  app.ts->screenright + (title_y + r) * fb_stride + btnX;
              unsigned short *dst = marquee.bg_strip + r * vis_w;
              memcpy(dst, src, vis_w * sizeof(unsigned short));
            }

            unsigned short *saved_screen = app.ts->GetScreen();
            int saved_mr = app.ts->margin.right;
            int saved_ml = app.ts->margin.left;
            const int off_fb_stride = app.ts->display.height;
            const int cap_w =
                (sw < app.ts->display.width) ? sw : app.ts->display.width;
            for (int r = 0; r < sh; r++)
              for (int c = 0; c < cap_w; c++)
                app.ts->offscreen[(title_y + r) * off_fb_stride + c] = 0xFFFF;

            app.ts->SetScreen(app.ts->offscreen);
            app.ts->margin.left = 0;
            app.ts->margin.right = 0;
            app.ts->SetPen(0, btnY + kTitleOffsetY);
            app.ts->PrintString(dname, cur_style);
            app.ts->margin.left = saved_ml;
            app.ts->margin.right = saved_mr;
            app.ts->SetScreen(saved_screen);

            for (int row_i = 0; row_i < sh; row_i++) {
              const unsigned short *src =
                  app.ts->offscreen + (title_y + row_i) * fb_stride;
              unsigned short *dst = marquee.strip + row_i * sw;
              int copy_w =
                  (sw < app.ts->display.width) ? sw : app.ts->display.width;
              memcpy(dst, src, copy_w * sizeof(unsigned short));
            }
            marquee.scroll_offset = 0;
            marquee.scroll_timer = 0;
            marquee.active = true;
          }
        } else if (is_selected && !overflows) {
          marquee.Reset();
        }

        if (!(is_selected && overflows)) {
          app.ts->SetPen(btnX, btnY + kTitleOffsetY);
          app.ts->PrintString(dname, cur_style);
        }

        app.ts->margin.right = saved_margin_right;
        app.ts->SetClipToContentEnabled(saved_clip);
        app.ts->SetAutoWrapEnabled(saved_wrap);
      }
    } else {
      browser_presentation_utils::DrawWrappedTitleInsideCover(
          app.ts, display_name, btnX + 2, btnY + 2, kCoverW, kCoverH,
          TEXT_STYLE_BROWSER);
    }

    int pos = app.books[i]->GetPosition();
    char msg[16];
    if (pos > 0)
      snprintf(msg, sizeof(msg), "Pg %d", pos + 1);
    else
      snprintf(msg, sizeof(msg), "NEW");
    app.ts->SetPen(btnX, btnY + kProgressOffsetY);
    app.ts->PrintString(msg);
  }
}

void TickMarquee(App &app, BrowserGridMarqueeState &marquee) {
  if (!marquee.active || !marquee.strip || !marquee.bg_strip)
    return;

  const int kPauseFrames = 60;
  const int scroll_max = marquee.strip_w - kCellW;

  if (marquee.scroll_timer < kPauseFrames) {
    marquee.scroll_timer++;
  } else if (marquee.scroll_offset >= scroll_max) {
    if (marquee.end_timer < kPauseFrames) {
      marquee.end_timer++;
    } else {
      marquee.scroll_offset = 0;
      marquee.scroll_timer = 0;
      marquee.end_timer = 0;
    }
  } else {
    marquee.scroll_offset++;
  }

  const int fb_stride = app.ts->display.height;
  const int tx = marquee.strip_x;
  const int ty = marquee.strip_y;
  const int sh = marquee.strip_h;
  const int sw = marquee.strip_w;
  const int off = marquee.scroll_offset;
  const int bw = marquee.blit_w;

  int blit_w = bw;
  if (blit_w + off > sw)
    blit_w = sw - off;
  if (blit_w <= 0)
    return;

  for (int row = 0; row < sh; row++) {
    if (ty + row >= app.ts->display.height)
      break;
    unsigned short *dst = app.ts->screenright + (ty + row) * fb_stride + tx;
    memcpy(dst, marquee.bg_strip + row * bw, bw * sizeof(unsigned short));
    const unsigned short *src = marquee.strip + row * sw + off;
    for (int col = 0; col < blit_w; col++) {
      if (src[col] != 0xFFFF)
        dst[col] = src[col];
    }
  }

  app.ts->MarkScreenDirtyRect(app.ts->screenright, tx, ty, tx + bw, ty + sh);
}

} // namespace browser_grid_view

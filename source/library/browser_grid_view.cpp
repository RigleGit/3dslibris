#include "library/browser_grid_view.h"

#include <cstring>

#include "app/app.h"
#include "book/book.h"
#include "debug_log.h"
#include "library/browser_presentation_hit_utils.h"
#include "library/browser_presentation_utils.h"
#include "ui/button.h"
#include "ui/text.h"

BrowserGridMarqueeState::BrowserGridMarqueeState()
    : book(NULL), strip(NULL), bg_strip(NULL), strip_w(0), strip_h(0),
      strip_x(0), strip_y(0), blit_w(0), scroll_offset(0), scroll_timer(0),
      end_timer(0), color_mode(-1), bg_color(0xFFFF), active(false) {}

void BrowserGridMarqueeState::Reset() {
  delete[] strip;
  delete[] bg_strip;
  strip = NULL;
  bg_strip = NULL;
  book = NULL;
  display_name.clear();
  strip_w = strip_h = strip_x = strip_y = blit_w = 0;
  scroll_offset = scroll_timer = end_timer = 0;
  color_mode = -1;
  bg_color = 0xFFFF;
  active = false;
}

namespace browser_grid_view {

namespace {

static bool RoundedRectContains(int x, int y, int w, int h, int radius) {
  if (w <= 0 || h <= 0)
    return false;
  if (radius <= 0)
    return x >= 0 && y >= 0 && x < w && y < h;
  if (x < 0 || y < 0 || x >= w || y >= h)
    return false;

  const int r = radius;
  if ((x >= r && x < w - r) || (y >= r && y < h - r))
    return true;

  int cx = (x < r) ? r - 1 : w - r;
  int cy = (y < r) ? r - 1 : h - r;
  const int dx = x - cx;
  const int dy = y - cy;
  return dx * dx + dy * dy <= r * r;
}

static void FitRectPreserveAspect(int src_w, int src_h, int box_w, int box_h,
                                  int *out_w, int *out_h) {
  if (!out_w || !out_h) {
    return;
  }
  if (src_w <= 0 || src_h <= 0 || box_w <= 0 || box_h <= 0) {
    *out_w = 0;
    *out_h = 0;
    return;
  }

  const long long lhs = (long long)box_w * (long long)src_h;
  const long long rhs = (long long)box_h * (long long)src_w;
  if (lhs <= rhs) {
    *out_w = box_w;
    *out_h = (int)((long long)src_h * (long long)box_w / (long long)src_w);
  } else {
    *out_h = box_h;
    *out_w = (int)((long long)src_w * (long long)box_h / (long long)src_h);
  }

  if (*out_w < 1)
    *out_w = 1;
  if (*out_h < 1)
    *out_h = 1;
}

} // namespace

int HitTestBookIndex(int x, int y, int page_start, int book_count) {
  return browser_presentation_hit_utils::HitTestGridBookIndex(
      x, y, page_start, book_count, kGridX0, kGridY0, kCellW, kCellH, kGridCols,
      kGridRows, APP_BROWSER_BUTTON_COUNT);
}

void DrawPage(App &app, BrowserGridMarqueeState &marquee, int page_start) {
  for (int i = page_start;
       i < app.BookCount() && i < page_start + APP_BROWSER_BUTTON_COUNT; i++) {
    int page_idx = i % APP_BROWSER_BUTTON_COUNT;
    int col = page_idx % kGridCols;
    int row = page_idx / kGridCols;
    int btnX = kGridX0 + col * kCellW;
    int btnY = kGridY0 + row * kCellH;

    app.buttons[i]->Draw(app.ts->screenright,
                         app.books[i] == app.GetSelectedBook());

    const bool has_cover = app.books[i]->coverPixels != NULL;

    if (has_cover) {
      const int inner_pad_x = 4;
      const int inner_pad_y = 4;
      const int inner_w = kCoverW - inner_pad_x * 2;
      const int inner_h = kCoverH - inner_pad_y * 2;
      int draw_w = 0;
      int draw_h = 0;
      FitRectPreserveAspect(app.books[i]->coverWidth, app.books[i]->coverHeight,
                            inner_w, inner_h, &draw_w, &draw_h);
      int cx = btnX + 2 + inner_pad_x + (inner_w - draw_w) / 2;
      int cy = btnY + 2 + inner_pad_y + (inner_h - draw_h) / 2;
      int w = app.ts->display.height;
      app.ts->MarkScreenDirtyRect(app.ts->screenright, cx, cy,
                                  cx + draw_w, cy + draw_h);
      for (int py = 0; py < draw_h && (cy + py) < 320; py++) {
        const int src_y =
            (int)((long long)py * (long long)app.books[i]->coverHeight /
                  (long long)draw_h);
        for (int px = 0; px < draw_w && (cx + px) < 240; px++) {
          const int src_x =
              (int)((long long)px * (long long)app.books[i]->coverWidth /
                    (long long)draw_w);
          if (!RoundedRectContains(px, py, draw_w, draw_h, 5))
            continue;
          app.ts->screenright[(cy + py) * w + (cx + px)] =
              app.books[i]
                  ->coverPixels[src_y * app.books[i]->coverWidth + src_x];
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
    if (has_cover) {
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

          int current_color_mode = app.ts->GetColorMode();
          if (marquee.book != book_i || marquee.display_name != display_name ||
              marquee.color_mode != current_color_mode) {
            const int vis_w = (btnX + kCellW <= app.ts->display.width)
                                  ? kCellW
                                  : app.ts->display.width - btnX;
            const int capture_w =
                (sw < app.ts->display.width) ? sw : app.ts->display.width;

            marquee.Reset();
            marquee.book = book_i;
            marquee.display_name = display_name;
            marquee.strip_w = capture_w;
            marquee.strip_h = sh;
            marquee.strip_x = btnX;
            marquee.strip_y = title_y;
            marquee.blit_w = vis_w;
            marquee.strip = new unsigned short[capture_w * sh];
            marquee.bg_strip = new unsigned short[vis_w * sh];
#ifdef DSLIBRIS_DEBUG
            if (book_i->GetStatusReporter()) {
              DBG_LOGF(book_i->GetStatusReporter(),
                       "MARQUEE build: dname='%.20s' full_w=%d sw=%d "
                       "capture_w=%d vis_w=%d btnX=%d title_y=%d sh=%d",
                       dname, full_w, sw, capture_w, vis_w, btnX, title_y,
                       sh);
            }
#endif

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
            int bg_pixels = vis_w * sh;
            if (bg_pixels > 0) {
              u32 r_acc = 0, g_acc = 0, b_acc = 0;
              for (int i = 0; i < bg_pixels; i++) {
                u16 c = marquee.bg_strip[i];
                r_acc += ((c >> 11) & 0x1F) << 3 | ((c >> 11) & 0x1F) >> 2;
                g_acc += ((c >> 5) & 0x3F) << 2 | ((c >> 5) & 0x3F) >> 4;
                b_acc += (c & 0x1F) << 3 | (c & 0x1F) >> 2;
              }
              marquee.bg_color = (u16)(((r_acc / bg_pixels) >> 3 << 11) |
                                       ((g_acc / bg_pixels) >> 2 << 5) |
                                       ((b_acc / bg_pixels) >> 3));
            }

            for (int r = 0; r < sh; r++) {
              for (int c = 0; c < capture_w; c++) {
                app.ts->offscreen[(title_y + r) * off_fb_stride + c] =
                    marquee.bg_color;
              }
            }

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
              unsigned short *dst = marquee.strip + row_i * capture_w;
              memcpy(dst, src, capture_w * sizeof(unsigned short));
            }
            marquee.scroll_offset = 0;
            marquee.scroll_timer = 0;
            marquee.color_mode = current_color_mode;
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
          app.ts.get(), display_name, btnX + 2, btnY + 2, kCoverW, kCoverH,
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
  const int scroll_max = marquee.strip_w - marquee.blit_w;

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
      if (src[col] != marquee.bg_color)
        dst[col] = src[col];
    }
  }

  app.ts->MarkScreenDirtyRect(app.ts->screenright, tx, ty, tx + bw, ty + sh);
}

} // namespace browser_grid_view

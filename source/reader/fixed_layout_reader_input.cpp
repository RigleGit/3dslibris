/*
    3dslibris - fixed_layout_reader_input.cpp
    New 3DS reader module by Rigle.

    Summary:
    - Input handling for fixed-layout books (PDF/CBZ/XPS) in reading mode.
    - Extracted from reader/app_book.cpp HandleEventInBook fixed-layout branch.
*/

#include "reader/fixed_layout_reader_input.h"

#include <math.h>
#include <3ds.h>

#include "app/app.h"
#include "book/book.h"
#include "book/book_renderer.h"
#include "formats/common/pdf_view_utils.h"
#include "reader/book_page_nav.h"
#include "reader/fixed_layout_input_utils.h"
#include "ui/text.h"

namespace {

static const int kPdfTouchRerenderDelta = 4;
static const int kViewportStickDeadZone = 15;
static const int kViewportCStickDeadZone = 8;
static const float kViewportStickBaseScale = 0.048f / 154.0f;
static const int kViewportStickAccelFrames = 24;

static float BuildViewportStickAxis(int raw_value, int dead_zone, float scale) {
  return (std::abs(raw_value) > dead_zone) ? ((float)raw_value * scale) : 0.0f;
}

static float BuildViewportStickAccel(int active_frames) {
  if (active_frames <= 0)
    return 1.0f;
  const int capped = (active_frames < kViewportStickAccelFrames)
                         ? active_frames
                         : kViewportStickAccelFrames;
  return 1.0f + ((float)capped / (float)kViewportStickAccelFrames);
}

static void MapViewportPanToReadingOrientation(bool turned_right,
                                               float physical_x,
                                               float physical_y,
                                               float *screen_dx,
                                               float *screen_dy) {
  if (!screen_dx || !screen_dy)
    return;
  if (!turned_right) {
    *screen_dx = physical_y;
    *screen_dy = -physical_x;
    return;
  }
  *screen_dx = -physical_y;
  *screen_dy = physical_x;
}

} // namespace

namespace fixed_layout_input {

bool HandleInBook(App &app, Book *book, Text *ts, uint32_t keys, uint32_t held,
                  uint16_t *pagecurrent, uint16_t pagecount,
                  const ReaderControls &ctrl) {
  bool status_dirty = false;

  const auto delay_deferred = [&]() {
    const uint32_t delay_ms = book_renderer::GetFixedLayoutDeferredDelayMs(book);
    app.SetPdfDeferredReadyAtMs(delay_ms ? (osGetTime() + delay_ms) : 0);
  };

  if (keys & ctrl.zoom_in) {
    if (book_renderer::ChangeFixedLayoutZoom(book, 1)) {
      book_nav::DrawPage(book, ts);
      status_dirty = true;
      delay_deferred();
    }
  } else if (keys & ctrl.zoom_out) {
    if (book_renderer::ChangeFixedLayoutZoom(book, -1)) {
      book_nav::DrawPage(book, ts);
      status_dirty = true;
      delay_deferred();
    }
  } else if (reader_input_utils::FixedLayoutSupportsShoulderPageTurn(book->format) &&
             (keys & (app.key.r | app.key.zl))) {
    if (book_nav::TurnPage(book, ts, pagecurrent, pagecount, 1)) {
      status_dirty = true;
      delay_deferred();
    }
  } else if (reader_input_utils::FixedLayoutSupportsShoulderPageTurn(book->format) &&
             (keys & (app.key.l | app.key.zr))) {
    if (book_nav::TurnPage(book, ts, pagecurrent, pagecount, -1)) {
      status_dirty = true;
      delay_deferred();
    }
  } else if (keys & ctrl.fixed_page_next) {
    if (book_nav::TurnPage(book, ts, pagecurrent, pagecount, 1)) {
      status_dirty = true;
      delay_deferred();
    }
  } else if (keys & ctrl.fixed_page_prev) {
    if (book_nav::TurnPage(book, ts, pagecurrent, pagecount, -1)) {
      status_dirty = true;
      delay_deferred();
    }
  } else if (keys & ctrl.fixed_chapter_next) {
    if (!book->GetChapters().empty()) {
      if (book_renderer::JumpFixedLayoutChapter(book, 1)) {
        book_renderer::ResetFixedLayoutViewportForNavigation(book);
        book_nav::DrawPage(book, ts);
        status_dirty = true;
        delay_deferred();
      }
    } else if (book_nav::TurnPage(book, ts, pagecurrent, pagecount, 1)) {
      status_dirty = true;
      delay_deferred();
    }
  } else if (keys & ctrl.fixed_chapter_prev) {
    if (!book->GetChapters().empty()) {
      if (book_renderer::JumpFixedLayoutChapter(book, -1)) {
        book_renderer::ResetFixedLayoutViewportForNavigation(book);
        book_nav::DrawPage(book, ts);
        status_dirty = true;
        delay_deferred();
      }
    } else if (book_nav::TurnPage(book, ts, pagecurrent, pagecount, -1)) {
      status_dirty = true;
      delay_deferred();
    }
  } else if (keys & KEY_TOUCH) {
    touchPosition mapped = app.TouchRead();
    app.SetPdfTouchDragActive(true);
    book_renderer::SetFixedLayoutViewportInteraction(book, true);
    app.SetPdfTouchLastX((int)mapped.px);
    app.SetPdfTouchLastY((int)mapped.py);
    if (book_renderer::MoveFixedLayoutViewportToPreview(book, (int)mapped.px,
                                                        (int)mapped.py)) {
      book_nav::DrawPage(book, ts);
      status_dirty = true;
      delay_deferred();
    }
  } else if (held & KEY_TOUCH) {
    touchPosition mapped = app.TouchRead();
    if (!app.IsPdfTouchDragActive() ||
        pdf_view_utils::TouchMovementExceedsThreshold(
            app.GetPdfTouchLastX(), app.GetPdfTouchLastY(), (int)mapped.px,
            (int)mapped.py, kPdfTouchRerenderDelta)) {
      app.SetPdfTouchDragActive(true);
      book_renderer::SetFixedLayoutViewportInteraction(book, true);
      app.SetPdfTouchLastX((int)mapped.px);
      app.SetPdfTouchLastY((int)mapped.py);
      if (book_renderer::MoveFixedLayoutViewportToPreview(book, (int)mapped.px,
                                                          (int)mapped.py)) {
        book_nav::DrawPage(book, ts);
        status_dirty = true;
        delay_deferred();
      }
    }
  } else if (keys & ctrl.back_to_library) {
    app.ShowLibraryView();
    app.prefs->Write();
  } else if (keys & ctrl.open_settings) {
    app.ShowSettingsView(true);
    app.prefs->Write();
  }

  if (!(held & KEY_TOUCH)) {
    if (app.IsPdfTouchDragActive()) {
      book_renderer::SetFixedLayoutViewportInteraction(book, false);
      book_nav::DrawPage(book, ts);
      status_dirty = true;
      delay_deferred();
    }
    book_renderer::SetFixedLayoutViewportInteraction(book, false);
    app.SetPdfTouchDragActive(false);
    app.SetPdfTouchLastX(-1);
    app.SetPdfTouchLastY(-1);
  }

  // Circle Pad / C-Stick smooth viewport pan.
  if (!app.IsPdfTouchDragActive()) {
    static int s_viewport_stick_active_frames = 0;
    circlePosition cpad;
    circlePosition cstick = {0, 0};
    hidCircleRead(&cpad);
    if (app.IsNew3dsDevice())
      hidCstickRead(&cstick);

    const float physical_x =
        BuildViewportStickAxis((int)cpad.dx, kViewportStickDeadZone,
                               kViewportStickBaseScale) +
        BuildViewportStickAxis((int)cstick.dx, kViewportCStickDeadZone,
                               kViewportStickBaseScale);
    const float physical_y =
        BuildViewportStickAxis(-(int)cpad.dy, kViewportStickDeadZone,
                               kViewportStickBaseScale) +
        BuildViewportStickAxis(-(int)cstick.dy, kViewportCStickDeadZone,
                               kViewportStickBaseScale);
    const bool stick_active = (physical_x != 0.0f || physical_y != 0.0f);
    s_viewport_stick_active_frames =
        stick_active ? (s_viewport_stick_active_frames + 1) : 0;

    float viewport_dx = 0.0f;
    float viewport_dy = 0.0f;
    const float accel = BuildViewportStickAccel(s_viewport_stick_active_frames);
    MapViewportPanToReadingOrientation(app.orientation, physical_x * accel,
                                       physical_y * accel, &viewport_dx,
                                       &viewport_dy);

    if (stick_active &&
        book_renderer::TranslateFixedLayoutViewport(book, viewport_dx, viewport_dy)) {
      book_renderer::SetFixedLayoutViewportInteraction(book, true);
      book_nav::DrawPage(book, ts);
      status_dirty = true;
      delay_deferred();
    }
  }

  if (!status_dirty &&
      !(held & KEY_TOUCH) && keys == 0 &&
      book_renderer::HasPendingFixedLayoutDeferredWork(book) &&
      osGetTime() >= app.GetPdfDeferredReadyAtMs()) {
    const uint32_t budget_ms = 4;
    const bool worked =
        book_renderer::PumpDeferredFixedLayoutWork(book, budget_ms);
    const uint32_t delay_ms = book_renderer::GetFixedLayoutDeferredDelayMs(book);
    app.SetPdfDeferredReadyAtMs(delay_ms ? (osGetTime() + delay_ms) : 0);
    if (worked) {
      book_nav::DrawPage(book, ts);
      status_dirty = true;
    }
  }

  return status_dirty;
}

} // namespace fixed_layout_input

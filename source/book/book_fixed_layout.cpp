#include "book/book.h"

#include "book/page.h"

void Book::DrawCurrentView(Text *ts) {
  if (!ts)
    return;
  if (IsPdf()) {
    DrawCurrentMuPdfView(ts);
    return;
  }
  if (IsCbz()) {
    DrawCurrentCbzView(ts);
    return;
  }
  if (GetPageCount() == 0)
    return;
  GetPage()->Draw(ts);
}

void Book::SetFixedLayoutViewportInteraction(bool active) {
  if (IsPdf()) {
    SetMuPdfViewportInteraction(active);
    return;
  }
  if (IsCbz())
    SetCbzViewportInteraction(active);
}

bool Book::ChangeFixedLayoutZoom(int delta) {
  if (IsPdf())
    return ChangeMuPdfZoom(delta);
  if (IsCbz())
    return ChangeCbzZoom(delta);
  return false;
}

bool Book::MoveFixedLayoutViewportToPreview(int touch_x, int touch_y) {
  if (IsPdf())
    return MoveMuPdfViewportToPreview(touch_x, touch_y);
  if (IsCbz())
    return MoveCbzViewportToPreview(touch_x, touch_y);
  return false;
}

bool Book::JumpFixedLayoutChapter(int delta) {
  if (IsPdf())
    return JumpMuPdfChapter(delta);
  if (IsCbz())
    return JumpCbzChapter(delta);
  return false;
}

bool Book::HasPendingFixedLayoutDeferredWork() const {
  if (IsPdf())
    return HasPendingMuPdfDeferredWork();
  if (IsCbz())
    return HasPendingCbzDeferredWork();
  return false;
}

u32 Book::GetFixedLayoutDeferredDelayMs() const {
  if (IsPdf())
    return GetMuPdfDeferredDelayMs();
  if (IsCbz())
    return GetCbzDeferredDelayMs();
  return 0;
}

bool Book::PumpDeferredFixedLayoutWork(u32 budget_ms) {
  if (IsPdf())
    return PumpDeferredMuPdfWork(budget_ms);
  if (IsCbz())
    return PumpDeferredCbzWork(budget_ms);
  return false;
}

void Book::CancelFixedLayoutDeferredWork() {
  if (IsPdf()) {
    CancelMuPdfIncrementalRender();
    return;
  }
  if (IsCbz())
    CancelCbzDeferredWork();
}

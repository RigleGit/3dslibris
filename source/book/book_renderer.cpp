#include "book/book_renderer.h"

#include "book/book.h"
#include "book/page.h"

#include <stddef.h>

namespace {

typedef bool (*BookRendererCanDrawFn)(Book *book);
typedef void (*BookRendererDrawFn)(Book *book, Text *text);

struct BookRendererEntry {
  const char *name;
  BookRendererCanDrawFn can_draw;
  BookRendererDrawFn draw;
};

bool CanDrawMuPdf(Book *book) { return book && book->IsPdf(); }

void DrawMuPdf(Book *book, Text *text) { book->DrawCurrentMuPdfView(text); }

bool CanDrawCbz(Book *book) { return book && book->IsCbz(); }

void DrawCbz(Book *book, Text *text) { book->DrawCurrentCbzView(text); }

bool CanDrawReflow(Book *book) {
  return book && !book->IsFixedLayout() && book->GetPageCount() > 0;
}

void DrawReflow(Book *book, Text *text) { book->GetPage()->Draw(text); }

static const BookRendererEntry kBookRenderers[] = {
    {"mupdf", CanDrawMuPdf, DrawMuPdf},
    {"cbz", CanDrawCbz, DrawCbz},
    {"reflow", CanDrawReflow, DrawReflow},
};

} // namespace

namespace book_renderer {

void DrawCurrentView(Book *book, Text *text) {
  if (!book || !text)
    return;
  for (size_t i = 0; i < sizeof(kBookRenderers) / sizeof(kBookRenderers[0]);
       i++) {
    const BookRendererEntry &entry = kBookRenderers[i];
    if (entry.can_draw(book)) {
      entry.draw(book, text);
      return;
    }
  }
}

void SetFixedLayoutViewportInteraction(Book *book, bool active) {
  if (!book)
    return;
  if (book->IsPdf()) {
    book->SetMuPdfViewportInteraction(active);
    return;
  }
  if (book->IsCbz())
    book->SetCbzViewportInteraction(active);
}

void ResetFixedLayoutViewportForNavigation(Book *book) {
  if (!book)
    return;
  if (book->IsPdf()) {
    book->ResetMuPdfViewport();
    return;
  }
  if (book->IsCbz())
    book->ResetCbzViewport();
}

bool ChangeFixedLayoutZoom(Book *book, int delta) {
  if (!book)
    return false;
  if (book->IsPdf())
    return book->ChangeMuPdfZoom(delta);
  if (book->IsCbz())
    return book->ChangeCbzZoom(delta);
  return false;
}

bool MoveFixedLayoutViewportToPreview(Book *book, int touch_x, int touch_y) {
  if (!book)
    return false;
  if (book->IsPdf())
    return book->MoveMuPdfViewportToPreview(touch_x, touch_y);
  if (book->IsCbz())
    return book->MoveCbzViewportToPreview(touch_x, touch_y);
  return false;
}

bool TranslateFixedLayoutViewport(Book *book, float dx, float dy) {
  if (!book)
    return false;
  if (book->IsPdf())
    return book->TranslateMuPdfViewport(dx, dy);
  if (book->IsCbz())
    return book->TranslateCbzViewport(dx, dy);
  return false;
}

bool JumpFixedLayoutChapter(Book *book, int delta) {
  if (!book)
    return false;
  if (book->IsPdf())
    return book->JumpMuPdfChapter(delta);
  if (book->IsCbz())
    return book->JumpCbzChapter(delta);
  return false;
}

bool HasPendingFixedLayoutDeferredWork(const Book *book) {
  if (!book)
    return false;
  if (book->IsPdf())
    return book->HasPendingMuPdfDeferredWork();
  if (book->IsCbz())
    return book->HasPendingCbzDeferredWork();
  return false;
}

u32 GetFixedLayoutDeferredDelayMs(const Book *book) {
  if (!book)
    return 0;
  if (book->IsPdf())
    return book->GetMuPdfDeferredDelayMs();
  if (book->IsCbz())
    return book->GetCbzDeferredDelayMs();
  return 0;
}

bool PumpDeferredFixedLayoutWork(Book *book, u32 budget_ms) {
  if (!book)
    return false;
  if (book->IsPdf())
    return book->PumpDeferredMuPdfWork(budget_ms);
  if (book->IsCbz())
    return book->PumpDeferredCbzWork(budget_ms);
  return false;
}

void CancelFixedLayoutDeferredWork(Book *book) {
  if (!book)
    return;
  if (book->IsPdf()) {
    book->CancelMuPdfIncrementalRender();
    return;
  }
  if (book->IsCbz())
    book->CancelCbzDeferredWork();
}

} // namespace book_renderer

#pragma once

class Book;
class Text;

#include <3ds/types.h>

namespace book_renderer {

// Transitional renderer boundary: kept during Book parser/renderer split.
//
// Renderer dispatch is selected once per Book::DrawCurrentView call, outside
// glyph/pixel hot loops.
void DrawCurrentView(Book *book, Text *text);
void SetFixedLayoutViewportInteraction(Book *book, bool active);
void ResetFixedLayoutViewportForNavigation(Book *book);
bool ChangeFixedLayoutZoom(Book *book, int delta);
bool MoveFixedLayoutViewportToPreview(Book *book, int touch_x, int touch_y);
bool TranslateFixedLayoutViewport(Book *book, float dx, float dy);
bool JumpFixedLayoutChapter(Book *book, int delta);
bool HasPendingFixedLayoutDeferredWork(const Book *book);
u32 GetFixedLayoutDeferredDelayMs(const Book *book);
bool PumpDeferredFixedLayoutWork(Book *book, u32 budget_ms);
void CancelFixedLayoutDeferredWork(Book *book);

} // namespace book_renderer

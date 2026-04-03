#pragma once

class App;
class Book;

class ReaderController {
public:
  explicit ReaderController(App &app);

  void CloseBook();
  int GetBookIndex(Book *book);
  void HandleEventInBook();
  void HandleEventInOpening();
  unsigned char OpenBook();
  void ToggleBookmark();

private:
  void ClearDeferredRelayoutState();
  bool MaybeFinalizeDeferredRelayout(Book *book, int page_count);

  App &app_;
};

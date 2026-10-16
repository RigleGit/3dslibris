#include "reader/search_session.h"
#include "reader/reader_controls.h"
#include "app/app_mode.h"
#include "shared/orientation_utils.h"
#include <cassert>
#include <algorithm>
#include <cstdio>

Button::Button() {}
enum { KEY_L=1, KEY_R=2, KEY_DLEFT=4, KEY_DRIGHT=8, KEY_DUP=16,
       KEY_DDOWN=32, KEY_TOUCH=64 };
struct FrameInput { u32 keys_down=0, keys_held=0; uint64_t timestamp_ms=0; };
struct TouchCandidates {};
namespace touch {
void BuildCandidates(touchPosition,TouchCandidates*) {}
bool HitsButton(const TouchCandidates&,Button*,int) { return false; }
}
struct Book {
  std::vector<std::vector<uint32_t>> pages{{'h','i','t'}, {'h','i','t'}, {'n','o'}, {'n','o'}, {'h','i','t'}};
  std::vector<page_search::Events> events{5};
  u16 position=0;
  unsigned GetOpenSessionId() { return 1; }
  unsigned GetLayoutRevision() { return 1; }
  unsigned GetPageCount() { return pages.size(); }
  int GetPosition() { return position; }
  void SetPosition(int p) { position=p; }
  bool IsAsyncReflowOpenPending() { return false; }
  unsigned char GetOrientation() { return 2; }
  bool IsCbz() { return false; }
  bool IsFixedLayout() { return false; }
  void ResetCbzFailureState() {}
  void ClearFocusedInlineLink() {}
  void ResetReadingPaceEstimate() {}
};
namespace text_search {
Source FromBook(Book *b) {
  return {b,(uint32_t)b->pages.size(),[](void *ctx,uint32_t i,PageView *out) {
    Book &book=*(Book*)ctx;
    if(i>=book.pages.size()) return false;
    *out={book.pages[i].data(),book.pages[i].size(),&book.events[i]}; return true;
  }};
}
}
namespace book_renderer {
void CancelFixedLayoutDeferredWork(Book*) {}
void ResetFixedLayoutViewportForNavigation(Book*) {}
}
namespace book_nav {
void DrawPage(Book*,Text*) {}
bool SetPage(Book*,Text*,uint16_t,bool);
bool TurnPage(Book*,Text*,uint16_t*,uint16_t,int,bool);
}
struct App {
  Book book;
  struct ScreenText : Text { void MarkAllScreensDirty() {} };
  std::unique_ptr<ScreenText> ts{new ScreenText};
  struct Keys {
    u32 a=128,b=256,x=512,start=1024,select=2048;
    u32 l=KEY_L,r=KEY_R,dleft=KEY_DLEFT,dright=KEY_DRIGHT,dup=KEY_DUP,ddown=KEY_DDOWN;
    u32 left=0,right=0,up=0,down=0,zl=0,zr=0;
  } key;
  AppMode mode=AppMode::Search;
  bool ShouldAbortWork() { return false; }
  Book *GetCurrentBook() { return &book; }
  void ShowCurrentBookView() { mode=AppMode::Book; }
  void ShowLibraryView() { mode=AppMode::Browser; }
  void ShowSettingsView(bool) { mode=AppMode::Prefs; }
  void SetMode(AppMode m) { mode=m; }
  void PresentIfDirty() {}
  touchPosition MapTouch(const FrameInput&) { return {}; }
};
struct ReaderController {
  App &app_;
  std::unique_ptr<ReaderSearchSession> search_{new ReaderSearchSession};
  ReaderController(App &app):app_(app) {
    auto &s=*search_; s.book=&app.book;s.session=s.revision=1;s.pages=5;
    s.active=true;s.wait_release=false;s.editor=false;s.query="hit";
  }
  void DrawSearch() { search_->dirty=false; }
  void InvalidateSearch(bool) { search_->active=false; }
  void MarkProgressDirty(Book*) {}
  void TryPersistProgress(Book*,bool) {}
  void StartSearch(bool,bool);
  void RunSearchFrame(const FrameInput&);
  void EditSearchText() {}
};
void LogSearch(App&,const ReaderSearchSession&,const char*) {}
#include "reader_search_under_test.inc"

int main() {
  App app;ReaderController reader(app);auto &s=*reader.search_;
  reader.StartSearch(false,true);
  while(s.engine.GetStatus()==text_search::Searching) reader.RunSearchFrame({});
  assert(s.has_match && app.book.position==0);
  FrameInput input;input.keys_down=KEY_DRIGHT;
  for(int i=0;i<3;++i) reader.RunSearchFrame(input);
  assert(app.book.position==3);
  input.keys_down=KEY_R;reader.RunSearchFrame(input);
  while(s.engine.GetStatus()==text_search::Searching) reader.RunSearchFrame({});
  assert(app.book.position==4 && !s.engine.Wrapped());
  // Inspecting the continuation of a cross-page match keeps its original anchor.
  app.book.pages={{'a','b'},{'c','d'},{'x'}};s.query="abcd";s.pages=3;
  app.book.position=0;reader.StartSearch(false,true);
  while(s.engine.GetStatus()==text_search::Searching) reader.RunSearchFrame({});
  assert(s.match.start.page==0 && s.match.end.page==1);
  reader.StartSearch(false,false); // cancel a still-pending traversal by paging
  input.keys_down=KEY_DRIGHT;reader.RunSearchFrame(input);
  assert(s.has_match && app.book.position==1);
  assert(s.message.find("Searching")==std::string::npos);
  input.keys_down=KEY_R;reader.RunSearchFrame(input);
  while(s.engine.GetStatus()==text_search::Searching) reader.RunSearchFrame({});
  assert(s.engine.GetStatus()==text_search::NotFound && app.book.position==1);
  puts("PASS: search navigation anchors, continuation and cancellation message");
}

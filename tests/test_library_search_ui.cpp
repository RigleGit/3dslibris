#include "app/app_mode.h"
#include "library/library_search_session.h"
#include "app/frame_input.h"
#include "shared/orientation_utils.h"
#include "shared/text_wrap_utils.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <cassert>
#include <cstdio>
#define DBG_LOGF(...) ((void)0)
Button::Button() {}
bool Button::EnclosesPoint(u16,u16) {return false;}
enum {KEY_DUP=1,KEY_DDOWN=2,KEY_DLEFT=4,KEY_DRIGHT=8,KEY_TOUCH=16};
struct Book {
  std::string name="filename.epub";
  bool IsBrowserFolder() const {return false;}
  const char *GetFileName() const {return name.c_str();}
  const char *GetFolderName() const {return "/library";}
  std::string GetBrowserFolderDisplayName() const {return "";}
};
struct App {
  std::unique_ptr<Text> ts{new Text};
  Book book;
  std::vector<Book*> books{40,&book};
  struct {u32 a=32,b=64,x=128,y=256,l=512,r=1024;} key;
  u8 portrait_orientation=0;
  AppMode mode=AppMode::LibrarySearch;
  bool abort=false;
  bool ShouldAbortWork() const {return abort;}
  void ShowLibraryView() {mode=AppMode::Browser;}
  void SetMode(AppMode value) {mode=value;}
  AppMode GetMode() const {return mode;}
  void PresentIfDirty() {}
};
static std::string EntryPath(Book *book) {return std::string(book->GetFolderName())+"/"+book->GetFileName();}
static uint64_t SearchClock() {return 0;}
struct SwkbdState {};
enum {SWKBD_TYPE_NORMAL,SWKBD_NOTEMPTY_NOTBLANK,SWKBD_BUTTON_LEFT,SWKBD_BUTTON_RIGHT,SWKBD_BUTTON_NONE};
enum {SWKBD_PREDICTIVE_INPUT=1,SWKBD_ALLOW_HOME=2,SWKBD_ALLOW_POWER=4};
typedef int SwkbdButton;
static int keyboard_result=SWKBD_BUTTON_RIGHT;
static u32 keyboard_features=0;
void swkbdInit(SwkbdState*,int,int,int) {}
void swkbdSetHintText(SwkbdState*,const char*) {}
void swkbdSetInitialText(SwkbdState*,const char*) {}
void swkbdSetValidation(SwkbdState*,int,int,int) {}
void swkbdSetButton(SwkbdState*,int,const char*,bool) {}
void swkbdSetFeatures(SwkbdState*,u32 flags) {keyboard_features=flags;}
int swkbdInputText(SwkbdState*,char *out,size_t size) {snprintf(out,size,"filename");return keyboard_result;}
int swkbdGetResult(SwkbdState*) {return 0;}
struct LibraryController {
  App &app_; bool inside_folder_=false;
  std::unique_ptr<LibrarySearchSession> search_{new LibrarySearchSession};
  unsigned draws=0,opened=0,closed=0;
  LibraryController(App &app):app_(app) {}
  static bool SearchRoot(void*,size_t,library_search::Entry*);
  static library_search::MetadataStatus SearchMetadata(void*,const std::string&,library_search::MetadataValue*) {assert(false && "Name search must not request metadata");return library_search::MetadataStatus::Error;}
  void StartLibrarySearch();void EditLibrarySearch();void RunLibrarySearchFrame(const FrameInput&);
  void DrawLibrarySearch() {++draws;search_->dirty=false;}
  void CloseLibrarySearch() {++closed;app_.ShowLibraryView();}
  void ActivateLibrarySearchResult(size_t i) {assert(i<search_->engine.Results().size());++opened;}
};
#include "library_search_ui.inc"

int main() {
  App app; LibraryController controller(app);auto &session=*controller.search_;
  FrameInput input;
  controller.RunLibrarySearchFrame(input); // release the key that opened search
  input.keys_down=app.key.a;controller.RunLibrarySearchFrame(input);
  assert(session.field==library_search::Field::Title);
  controller.RunLibrarySearchFrame(input);assert(session.field==library_search::Field::Author);
  controller.RunLibrarySearchFrame(input);assert(session.field==library_search::Field::Name);
  input.keys_down=app.key.y;controller.RunLibrarySearchFrame(input);
  assert(session.query=="filename" && session.selected==2);
  assert(keyboard_features==(SWKBD_PREDICTIVE_INPUT|SWKBD_ALLOW_HOME|SWKBD_ALLOW_POWER));
  input.keys_down=0;controller.RunLibrarySearchFrame(input);
  input.keys_down=app.key.a;controller.RunLibrarySearchFrame(input);
  assert(!session.editor);
  input.keys_down=0;controller.RunLibrarySearchFrame(input);
  assert(session.engine.Results().size()==16);
  input.keys_down=app.key.b;controller.RunLibrarySearchFrame(input);
  assert(session.editor && !controller.closed);
  keyboard_result=SWKBD_BUTTON_LEFT;
  input.keys_down=app.key.y;controller.RunLibrarySearchFrame(input);
  assert(session.query=="filename");
  input.keys_down=0;controller.RunLibrarySearchFrame(input);
  input.keys_down=app.key.b;controller.RunLibrarySearchFrame(input);
  assert(controller.closed==1 && app.mode==AppMode::Browser);
  // Search again to get a result list for the page jumps.
  input.keys_down=0;controller.RunLibrarySearchFrame(input);
  session.selected=2;input.keys_down=app.key.a;controller.RunLibrarySearchFrame(input);
  input.keys_down=0;controller.RunLibrarySearchFrame(input);
  assert(!session.editor && session.engine.Results().size()>=16);
  // L/R jump between pages of cumulative-height pagination (variable row count).
  session.page_starts={0,4,9,12};session.result_selection=5;
  input.keys_down=app.key.l;controller.RunLibrarySearchFrame(input);assert(session.result_selection==0);
  input.keys_down=app.key.r;controller.RunLibrarySearchFrame(input);assert(session.result_selection==4);
  controller.RunLibrarySearchFrame(input);assert(session.result_selection==9);
  controller.RunLibrarySearchFrame(input);assert(session.result_selection==12);
  controller.RunLibrarySearchFrame(input);assert(session.result_selection==session.engine.Results().size()-1);
  input.keys_down=app.key.l;controller.RunLibrarySearchFrame(input);assert(session.result_selection==9);
  assert((int)AppMode::LibrarySearch==14);
  puts("PASS: real library-search input/editor, native keyboard cancel, name isolation and cooperative cancellation");
}

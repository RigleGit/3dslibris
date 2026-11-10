#include "library/library_search_session.h"
#include "app/app_mode.h"
#include "shared/path_utils.h"
#include "shared/path_constants.h"
#include "shared/orientation_utils.h"
#define DBG_LOG(...) ((void)0)
struct App;
namespace reader_internal {void EnsureBookMode(App *,const char *);}
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
Button::Button() {}
struct Book {
  std::string path;bool directory;
  Book(const std::string &p,bool dir):path(p),directory(dir) {}
};
static std::string EntryPath(Book *book) {return book->path;}
static std::string Parent(const std::string &p) {return p.substr(0,p.find_last_of('/'));}
static bool Exists(const std::string &p,bool dir) {struct stat st;return !stat(p.c_str(),&st) && (dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));}
struct App {
  std::string bookdir;
  std::vector<Book*> books;
  Book *selected=nullptr,*current=nullptr;
  struct {u8 orientation=0;} reader_layout;
  int page=10;
  bool open_ok=false;
  bool open_async=false; // EPUB/MOBI/FB2 return 0 while parsing in Opening mode.
  u8 portrait_orientation=0;
  bool ShouldAbortWork() const {return false;}
  void ApplyRenderOrientation(u8) {}
  AppMode mode=AppMode::LibrarySearch;
  void SetSelectedBook(Book *book) {selected=book;}
  Book *GetSelectedBook() const {return selected;}
  void SetBrowserPageStart(int value) {page=value;}
  void SetMode(AppMode value) {mode=value;}
  AppMode GetMode() const {return mode;}
  void ShowLibraryView() {mode=AppMode::Browser;}
  void ShowCurrentBookView() {if(current) mode=AppMode::Book;}
  int OpenBook() {
    if(open_async) {current=selected;mode=AppMode::Opening;return 0;}
    if(open_ok) {
      current=selected;
      // Use the actual reader transition gate, rather than supplying its result.
      reader_internal::EnsureBookMode(this,nullptr);
    }
    return open_ok ? 0 : 1;
  }
};
namespace reader_internal {
#include "ensure_book_mode.inc"
}
struct LibraryController {
  App &app_;bool inside_folder_=false;
  std::string current_folder_path_;
  std::unique_ptr<LibrarySearchSession> search_{new LibrarySearchSession};
  std::vector<Book*> roots,children;
  LibraryController(App &app):app_(app) {}
  void RebuildRoot() {inside_folder_=false;current_folder_path_.clear();app_.books=roots;}
  void LoadFolderPath(const std::string &path,const std::string &) {inside_folder_=true;current_folder_path_=path;app_.books=children;}
  void EnterFolder(Book *folder) {LoadFolderPath(folder->path,"");}
  void RestoreSearchOrigin();void ActivateLibrarySearchResult(size_t);void CloseLibrarySearch();
};
#include "search_navigation.inc"
struct Source {std::vector<library_search::Entry> entries;};
static bool Root(void *ctx,size_t i,library_search::Entry *out) {auto &s=*(Source*)ctx;if(i>=s.entries.size()) return false;*out=s.entries[i];return true;}
int main() {
  char tmp[]="/tmp/search-nav-XXXXXX";const std::string path=mkdtemp(tmp),folder=path+"/Folder";
  mkdir(folder.c_str(),0700);std::ofstream(path+"/root.epub")<<"root";std::ofstream(folder+"/child.epub")<<"child";
  Book root{path+"/root.epub",false},child{folder+"/child.epub",false},directory{folder,true};
  App app;app.bookdir=path;app.books={&root,&directory};app.selected=&root;
  LibraryController owner(app);owner.roots=app.books;owner.children={&child};
  auto &s=*owner.search_;s.query="child";s.origin_selection=root.path;s.origin_page=10;
  Source candidates{{{child.path,"child.epub",false}}};
  assert(s.engine.Start(library_search::Field::Name,s.query,Root,&candidates,false));s.engine.Pump();
  owner.ActivateLibrarySearchResult(0); // Failure restores origin and keeps exact results/query.
  assert(owner.search_ && s.query=="child" && s.engine.Results().size()==1);
  assert(!owner.inside_folder_ && app.selected==&root && app.page==10 && app.mode==AppMode::LibrarySearch);
  app.open_ok=true;owner.ActivateLibrarySearchResult(0);
  assert(!owner.search_ && owner.inside_folder_ && app.selected==&child && app.mode==AppMode::Book);
  assert(app.current==&child);
  // Reflow formats return 0 and leave the app in Opening mode; that is a successful open.
  owner.search_.reset(new LibrarySearchSession);app.mode=AppMode::LibrarySearch;app.open_ok=false;app.open_async=true;
  candidates.entries={{child.path,"child.epub",false}};
  assert(owner.search_->engine.Start(library_search::Field::Name,"child",Root,&candidates,false));
  owner.search_->engine.Pump();owner.ActivateLibrarySearchResult(0);
  assert(!owner.search_ && app.mode==AppMode::Opening && app.current==&child);
  app.open_async=false;app.open_ok=true;
  // Root results must use the same reader transition while retaining virtual root.
  owner.search_.reset(new LibrarySearchSession);app.mode=AppMode::LibrarySearch;
  candidates.entries={{root.path,"root.epub",false}};
  assert(owner.search_->engine.Start(library_search::Field::Name,"root",Root,&candidates,false));
  owner.search_->engine.Pump();owner.ActivateLibrarySearchResult(0);
  assert(!owner.search_ && !owner.inside_folder_ && app.current==&root && app.mode==AppMode::Book);
  owner.LoadFolderPath(folder,"");
  owner.search_.reset(new LibrarySearchSession);auto &back=*owner.search_;
  back.origin_folder=folder;back.origin_selection=child.path;
  unlink(child.path.c_str());rmdir(folder.c_str());
  owner.CloseLibrarySearch();
  assert(!owner.inside_folder_ && app.mode==AppMode::Browser && "missing original folder must fall back to virtual root");
  unlink(root.path.c_str());rmdir(path.c_str());
  puts("PASS: real search result opening, failure recovery, retained results and vanished origin folder");
}

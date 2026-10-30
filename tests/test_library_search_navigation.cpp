#include "library/library_search_session.h"
#include "app/app_mode.h"
#include "shared/path_utils.h"
#include "shared/path_constants.h"
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
  Book *selected=nullptr;
  int page=10;
  bool open_ok=false;
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
  int OpenBook() {if(open_ok) mode=AppMode::Book;return open_ok ? 0 : 1;}
};
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
  owner.search_.reset(new LibrarySearchSession);auto &back=*owner.search_;
  back.origin_folder=folder;back.origin_selection=child.path;
  unlink(child.path.c_str());rmdir(folder.c_str());
  owner.CloseLibrarySearch();
  assert(!owner.inside_folder_ && app.mode==AppMode::Browser && "missing original folder must fall back to virtual root");
  unlink(root.path.c_str());rmdir(path.c_str());
  puts("PASS: real search result opening, failure recovery, retained results and vanished origin folder");
}

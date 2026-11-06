#include "shared/app_flow_utils.h"
#include "shared/path_constants.h"
#include "shared/utf8_utils.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// Filesystem/traversal and saved selection are production code. Native 3DS
// services are unavailable here; these fixtures exercise the POSIX backend.
typedef int Result;
typedef unsigned Handle;
typedef unsigned short u16;
typedef unsigned u32;
struct FS_Archive {};
struct FS_DirectoryEntry {u16 name[256];u32 attributes;};
enum {ARCHIVE_SDMC,PATH_EMPTY,PATH_ASCII,FS_ATTRIBUTE_DIRECTORY=1};
static bool R_FAILED(Result rc) {return rc!=0;}
static int fsMakePath(int,const char *) {return 0;}
static int FSUSER_OpenArchive(FS_Archive*,int,int) {assert(false);return -1;}
static int FSUSER_OpenDirectory(Handle*,FS_Archive,int) {assert(false);return -1;}
static int FSUSER_CloseArchive(FS_Archive) {assert(false);return -1;}
static int FSDIR_Read(Handle,u32*,int,FS_DirectoryEntry*) {assert(false);return -1;}
static int FSDIR_Close(Handle) {assert(false);return -1;}
static std::string SdmcToArchiveRelPath(const std::string &p) {return p;}
static bool Utf16NameToUtf8(const u16 *,std::string *) {assert(false);return false;}
namespace cover_cache {static void ClearPathCache() {}}
struct Text {};
struct Book {
  std::string folder,name;bool directory;
  const char *GetFolderName() {return folder.c_str();}
  const char *GetFileName() {return name.c_str();}
  bool IsBrowserFolder() {return directory;}
};
struct App {
  std::string bookdir;
  std::unique_ptr<Text> ts{new Text};unsigned char colorMode=0;
  std::vector<Book*> books;Book *selected=nullptr;
  ~App() {Clear();}
  void Clear() {for(Book *book:books) delete book;books.clear();selected=nullptr;}
  void SetSelectedBook(Book *book) {selected=book;}
};
struct LibraryGradientContext {Text *ts;const unsigned char *color_mode;};
static void AppendBookFromFilename(App *app,LibraryGradientContext*,const std::string &dir,const char *name,std::set<std::string>*) {
  if(app_flow_utils::ShouldIndexBookFilename(name)) app->books.push_back(new Book{dir,name,false});
}
static void AppendFolderEntry(App *app,const std::string &dir,const std::string &name,LibraryGradientContext*,std::set<std::string>*) {
  if(!name.empty() && name[0]!='.') app->books.push_back(new Book{dir,name,true});
}
struct LibraryController {
  App &app_;LibraryGradientContext gradient_ctx_;unsigned folder_loads=0;
  explicit LibraryController(App &app):app_(app) {}
  int FindBooks();
  Book *RestoreSavedBookSelection(const char *,const char *);
  void LoadFolderPath(const std::string &path,const std::string &) {
    ++folder_loads;app_.Clear();
    DIR *dir=opendir(path.c_str());assert(dir);
    while(dirent *entry=readdir(dir)) AppendBookFromFilename(&app_,nullptr,path,entry->d_name,nullptr);
    closedir(dir);
  }
  void RebuildRoot() {app_.Clear();FindBooks();}
};
#include "library_source.inc"
static void Write(const std::string &path) {std::ofstream file(path);assert(file);file<<"book";}
static bool Contains(const App &app,const char *name) {
  for(Book *book:app.books) if(book->name==name) return true;
  return false;
}
int main() {
  const std::string root=std::getenv("LIBRARY_SOURCE_TEST_DIR");assert(!chdir(root.c_str()));
  mkdir("romfs:",0700);mkdir("romfs:/3ds",0700);mkdir("romfs:/3ds/3dslibris",0700);
  const std::string romfs=paths::kRomfsBookDir,sd=root+"/sd-books";
  mkdir(romfs.c_str(),0700);Write(romfs+"/README.md");Write(romfs+"/QuickStart.txt");
  const std::string bundled_folder=romfs+"/Samples";mkdir(bundled_folder.c_str(),0700);Write(bundled_folder+"/sample.epub");
  App app;app.bookdir=sd;LibraryController owner(app);
  assert(owner.FindBooks()==0 && Contains(app,"QuickStart.txt")); // Missing SD folder falls back.
  app.Clear();mkdir(sd.c_str(),0700);
  assert(owner.FindBooks()==0 && app.books.empty() && "an existing empty SD library must not resurrect bundled books");
  Write(sd+"/novel.epub");Write(sd+"/README.md");Write(sd+"/QuickStart.txt");
  app.Clear();assert(owner.FindBooks()==0 && app.books.size()==3);
  for(Book *book:app.books) assert(book->folder==sd);
  unlink((sd+"/README.md").c_str());unlink((sd+"/QuickStart.txt").c_str());
  app.Clear();assert(owner.FindBooks()==0 && app.books.size()==1 && Contains(app,"novel.epub"));
  assert(!owner.RestoreSavedBookSelection(romfs.c_str(),"QuickStart.txt") && owner.folder_loads==0);
  assert(!owner.RestoreSavedBookSelection(bundled_folder.c_str(),"sample.epub") && owner.folder_loads==0);
  assert(!owner.RestoreSavedBookSelection(nullptr,"sample.epub") && owner.folder_loads==0);
  const std::string child=sd+"/García";mkdir(child.c_str(),0700);Write(child+"/child.epub");
  Book *restored=owner.RestoreSavedBookSelection(child.c_str(),"child.epub");
  assert(restored && restored==app.selected && owner.folder_loads==1);
  app.Clear();owner.FindBooks();
  restored=owner.RestoreSavedBookSelection(nullptr,"child.epub");
  assert(restored && restored==app.selected && owner.folder_loads==2);
  assert(!owner.RestoreSavedBookSelection(child.c_str(),"missing.epub"));
  // A legacy folder path must still work when the configured root gains a slash.
  app.Clear();app.bookdir=sd+"/";owner.FindBooks();
  restored=owner.RestoreSavedBookSelection(child.c_str(),"child.epub");
  assert(restored && restored==app.selected);
  // Explicit ROMFS libraries and their folder restore remain supported.
  app.Clear();app.bookdir=romfs;assert(owner.FindBooks()==0 && Contains(app,"QuickStart.txt"));
  restored=owner.RestoreSavedBookSelection(nullptr,"sample.epub");
  assert(restored && restored==app.selected);
  // If the configured path is not a directory, internal onboarding remains available.
  app.Clear();app.bookdir=root+"/not-a-folder";Write(app.bookdir);assert(owner.FindBooks()==0 && Contains(app,"QuickStart.txt"));
  puts("PASS: actual root scan respects SD deletions/empty directories; ROMFS fallback and saved-folder restoration");
}

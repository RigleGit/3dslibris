#include "library/library_walk.h"
#include "shared/app_flow_utils.h"
#include "shared/utf8_utils.h"
#include <dirent.h>
#include <new>
#include <sys/stat.h>
#ifdef __3DS__
#include <3ds.h>
#endif
namespace library_walk {
namespace {
struct Directory {
  DIR *posix=nullptr;
  std::string path;
#ifdef __3DS__
  FS_Archive archive{};
  Handle handle=0;
  bool archive_open=false;
  FS_DirectoryEntry batch[16];
  u32 index=0,count=0;
#endif
  ~Directory() { Close(); }
  void Close() {
    if(posix) {closedir(posix);posix=nullptr;}
#ifdef __3DS__
    if(handle) {FSDIR_Close(handle);handle=0;}
    if(archive_open) {FSUSER_CloseArchive(archive);archive_open=false;}
    index=count=0;
#endif
    path.clear();
  }
  bool Open(const std::string &name) {
    Close(); path=name;
#ifdef __3DS__
    if(name.compare(0,6,"sdmc:/")==0 &&
       R_SUCCEEDED(FSUSER_OpenArchive(&archive,ARCHIVE_SDMC,fsMakePath(PATH_EMPTY,"")))) {
      archive_open=true;
      const std::string relative=name.substr(5);
      if(R_SUCCEEDED(FSUSER_OpenDirectory(&handle,archive,fsMakePath(PATH_ASCII,relative.c_str())))) return true;
      FSUSER_CloseArchive(archive); archive_open=false;handle=0;
    }
#endif
    posix=opendir(name.c_str());
    return posix!=nullptr;
  }
  bool Active() const {
#ifdef __3DS__
    if(handle) return true;
#endif
    return posix!=nullptr;
  }
  int Next(Entry *entry) {
#ifdef __3DS__
    if(handle) {
      if(index==count) {
        index=count=0;
        if(R_FAILED(FSDIR_Read(handle,&count,16,batch))) return -1;
        if(!count) return 0;
      }
      const FS_DirectoryEntry &value=batch[index++];
      if(!utf8_utils::Utf16NameToUtf8(value.name,&entry->name)) {entry->name.clear();return 1;}
      entry->directory=(value.attributes&FS_ATTRIBUTE_DIRECTORY)!=0;
      entry->path=path+"/"+entry->name;
      return 1;
    }
#endif
    dirent *value=readdir(posix);
    if(!value) return 0;
    entry->name=value->d_name; entry->path=path+"/"+entry->name;
    struct stat st;
    // lstat deliberately excludes links in the POSIX fallback/host backend.
    if(lstat(entry->path.c_str(),&st)) return -1;
    entry->directory=S_ISDIR(st.st_mode);
    if(!entry->directory && !S_ISREG(st.st_mode)) entry->name.clear();
    return 1;
  }
};
}
struct Cursor::State {
  RootSource roots=nullptr;void *context=nullptr;
  size_t index=0,errors=0;bool children=false;
  Directory directory;
};
Cursor::Cursor():state_(new(std::nothrow) State) {}
Cursor::~Cursor() {}
bool Cursor::Start(RootSource roots,void *context,bool children) {
  Close();if(!state_ || !roots) return false;
  state_->roots=roots;state_->context=context;state_->children=children;
  state_->index=state_->errors=0;return true;
}
void Cursor::Close() {if(state_) {state_->directory.Close();state_->roots=nullptr;}}
size_t Cursor::Errors() const {return state_ ? state_->errors : 0;}
Step Cursor::Next(Entry *entry) {
  if(!state_ || !state_->roots || !entry) return Step::End;
  auto &s=*state_;
  if(s.directory.Active()) {
    const int rc=s.directory.Next(entry);
    if(rc<=0) {if(rc<0) ++s.errors;s.directory.Close();return Step::Skip;}
    if(entry->directory) return Step::Skip;
  } else {
    if(!s.roots(s.context,s.index++,entry)) {Close();return Step::End;}
    if(entry->directory && s.children && !s.directory.Open(entry->path)) ++s.errors;
  }
  if(entry->name.empty() || entry->name[0]=='.' ||
     (!entry->directory && !app_flow_utils::ShouldIndexBookFilename(entry->name.c_str()))) return Step::Skip;
  return Step::Item;
}
}

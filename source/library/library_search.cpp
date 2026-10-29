#include "library/library_search.h"
#include "shared/app_flow_utils.h"
#include "shared/utf8_utils.h"
#include "utf8proc.h"
#include <algorithm>
#include <dirent.h>
#include <new>
#include <sys/stat.h>
#ifdef __3DS__
#include <3ds.h>
#endif

namespace library_search {
namespace {
const size_t kMemoryLimit=256u*1024u;
bool Normalize(const std::string &in,std::string *out) {
  out->clear();
  for(size_t i=0;i<in.size();) {
    int32_t cp,parts[32];
    const auto n=utf8proc_iterate((const uint8_t*)in.data()+i,in.size()-i,&cp);
    if(n<=0) return false;
    i+=(size_t)n;
    const auto count=utf8proc_decompose_char(cp,parts,32,
        (utf8proc_option_t)(UTF8PROC_CASEFOLD|UTF8PROC_DECOMPOSE|UTF8PROC_STABLE),nullptr);
    if(count<0 || count>32) return false;
    for(int j=0;j<count;++j) {
      const auto cat=utf8proc_category(parts[j]);
      if(cat==UTF8PROC_CATEGORY_MN || cat==UTF8PROC_CATEGORY_MC || cat==UTF8PROC_CATEGORY_ME) continue;
      if(parts[j]<=32 || cat==UTF8PROC_CATEGORY_ZS || cat==UTF8PROC_CATEGORY_ZL || cat==UTF8PROC_CATEGORY_ZP) {
        if(!out->empty() && out->back()!=' ') out->push_back(' ');
      } else {
        uint8_t bytes[4]; const int len=utf8proc_encode_char(parts[j],bytes);
        if(len<=0) return false;
        out->append((const char*)bytes,(size_t)len);
      }
      if(out->size()>32768) return false;
    }
  }
  if(!out->empty() && out->back()==' ') out->pop_back();
  return true;
}

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
      FSUSER_CloseArchive(archive); archive_open=false;
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
} // namespace

struct Engine::State {
  Field field=Field::Name;
  Status status=Status::Idle;
  RootSource roots=nullptr;
  MetadataSource metadata=nullptr;
  void *context=nullptr;
  bool child_files=false;
  size_t next_root=0,examined=0,errors=0,unsupported=0,uncached=0,bytes=0;
  std::vector<std::string> terms;
  std::vector<Result> results;
  Directory directory;
  bool Match(const std::string &value) {
    std::string normalized;
    if(!Normalize(value,&normalized)) {++errors;return false;}
    for(const auto &term:terms) if(normalized.find(term)==std::string::npos) return false;
    return true;
  }
  bool Process(const Entry &entry) {
    if(entry.name.empty() || entry.name[0]=='.') return false;
    if(!entry.directory && !app_flow_utils::ShouldIndexBookFilename(entry.name.c_str())) return false;
    ++examined;
    MetadataValue value;
    std::string label=entry.name;
    if(field!=Field::Name) {
      if(entry.directory) return false;
      const MetadataStatus rc=metadata ? metadata(context,entry.path,&value) : MetadataStatus::Unsupported;
      if(rc==MetadataStatus::Cancelled) {status=Status::Cancelled;return true;}
      if(rc==MetadataStatus::Unsupported) ++unsupported;
      else if(rc==MetadataStatus::Error) ++errors;
      if(rc!=MetadataStatus::Ready) return value.extracted;
      if(!value.cache_saved) ++uncached;
      if(!value.title.empty()) label=value.title;
    }
    const std::string &text=field==Field::Title ? value.title : field==Field::Author ? value.author : entry.name;
    if(Match(text)) {
      Result result;
      result.path=entry.path;result.name=entry.name;result.directory=entry.directory;
      result.label=label;result.author=value.author;
      const size_t cost=sizeof(Result)+result.path.capacity()+result.name.capacity()+result.label.capacity()+result.author.capacity()+4;
      if(results.size()>=256 || cost>kMemoryLimit-32768 || bytes>kMemoryLimit-32768-cost) status=Status::Limited;
      else {bytes+=cost;results.push_back(std::move(result));}
    }
    return value.extracted;
  }
};
Engine::Engine():state_(new(std::nothrow) State) {}
Engine::~Engine() {}
bool Engine::Start(Field field,const std::string &query,RootSource roots,void *context,
                   bool child_files,MetadataSource metadata) {
  if(!state_) return false;
  Cancel(); State &s=*state_;
  s.results.clear(); s.terms.clear();s.bytes=0;
  s.next_root=s.examined=s.errors=s.unsupported=s.uncached=0;
  std::string normalized;
  if(!roots || query.size()>512 || !Normalize(query,&normalized) || normalized.empty()) {s.status=Status::Idle;return false;}
  size_t start=0;
  while(start<normalized.size()) {
    const size_t end=normalized.find(' ',start);
    s.terms.push_back(normalized.substr(start,end==std::string::npos ? end : end-start));
    if(end==std::string::npos) break;
    start=end+1;
  }
  s.field=field;s.roots=roots;s.context=context;s.child_files=child_files;s.metadata=metadata;
  s.status=Status::Searching;return true;
}
void Engine::Pump(unsigned entries,Clock clock,unsigned milliseconds) {
  if(!state_ || state_->status!=Status::Searching) return;
  State &s=*state_; const uint64_t start=clock ? clock() : 0;
  for(unsigned i=0;i<entries && s.status==Status::Searching;++i) {
    if(i && clock && clock()-start>=milliseconds) break;
    Entry entry;
    if(s.directory.Active()) {
      const int rc=s.directory.Next(&entry);
      if(rc<=0) {if(rc<0) ++s.errors;s.directory.Close();continue;}
      if(entry.directory) continue;
    } else {
      if(!s.roots(s.context,s.next_root++,&entry)) {s.status=Status::Complete;break;}
      if(entry.directory && s.child_files && !s.directory.Open(entry.path)) ++s.errors;
    }
    if(s.Process(entry)) break; // At most one extraction per main-loop tick.
  }
  if(s.status!=Status::Searching) s.directory.Close();
}
void Engine::Cancel() {if(state_) {state_->directory.Close();state_->status=Status::Cancelled;}}
Status Engine::GetStatus() const {return state_ ? state_->status : Status::Idle;}
const std::vector<Result> &Engine::Results() const {static const std::vector<Result> empty;return state_ ? state_->results : empty;}
size_t Engine::Examined() const {return state_ ? state_->examined : 0;}
size_t Engine::Errors() const {return state_ ? state_->errors : 0;}
size_t Engine::Unsupported() const {return state_ ? state_->unsupported : 0;}
size_t Engine::Uncached() const {return state_ ? state_->uncached : 0;}
size_t Engine::AuxiliaryBytes() const {return state_ ? state_->bytes+32768 : 0;}
} // namespace library_search

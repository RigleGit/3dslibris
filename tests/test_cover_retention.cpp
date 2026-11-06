#include "test_folder_cover_support.h"
#include <cstddef>
static size_t live_bytes=0;
union Allocation {size_t size;std::max_align_t alignment;};
void *operator new(size_t size) {auto *p=(Allocation*)malloc(sizeof(Allocation)+size);if(!p) abort();p->size=size;live_bytes+=size;return p+1;}
void operator delete(void *ptr) noexcept {if(ptr) {auto *p=(Allocation*)ptr-1;live_bytes-=p->size;free(p);}}
void *operator new[](size_t n) {return ::operator new(n);}
void operator delete[](void *p) noexcept {::operator delete(p);}
namespace cover_decode_utils {bool DecodeImageToCoverThumb(Book *,const unsigned char *,size_t,int) {assert(false && "cached-only read must not decode");return false;}}
int main() {
  const std::string root=getenv("FOLDER_COVER_TEST_ROOT"),mode=getenv("RETENTION_TEST");
  if(mode=="failure") {
    mkdir(paths::GetCacheBaseDir().c_str(),0777);mkdir(paths::GetCoverCacheDir().c_str(),0777);
    mkdir(paths::GetCoverRetentionPath().c_str(),0777);
    assert(!cover_cache::EnableFullRetention());return 0;
  }
  if(mode=="absent") {
    Book book;const auto path=root+"/nocover.epub";
    book.SetBrowserFolderEntry(root,"folder",path);
    assert(!cover_cache::IsMarkedAbsent(&book,path));
    assert(cover_cache::MarkAbsent(&book,path));
    assert(cover_cache::IsMarkedAbsent(&book,path));
    // The marker is not a thumbnail and must not be treated as corrupt data.
    assert(!cover_cache::TryLoadCachedOnly(&book,path) && !book.coverPixels);
    std::string marker;DIR *dir=opendir(paths::GetCoverCacheDir().c_str());assert(dir);
    while(auto *entry=readdir(dir)) if(HasExtCI(entry->d_name,".cvr")) marker=paths::GetCoverCacheDir()+"/"+entry->d_name;
    closedir(dir);assert(!marker.empty());
    // A partial marker is ignored rather than trusted.
    assert(truncate(marker.c_str(),3)==0);assert(!cover_cache::IsMarkedAbsent(&book,path));
    assert(cover_cache::MarkAbsent(&book,path)&&cover_cache::IsMarkedAbsent(&book,path));
    // A real thumbnail saved later replaces the marker.
    book.coverWidth=85;book.coverHeight=115;book.coverPixels=new u16[85*115];
    std::fill(book.coverPixels,book.coverPixels+85*115,(u16)7);
    assert(cover_cache::Save(&book,path));assert(!cover_cache::IsMarkedAbsent(&book,path));
    Book again;again.SetBrowserFolderEntry(root,"folder",path);
    assert(cover_cache::TryLoadCachedOnly(&again,path)&&again.coverPixels[0]==7);
    puts("PASS: absent-cover marker is keyed, ignored when partial and replaced by a thumbnail");return 0;
  }
  if(mode=="write") {
    assert(cover_cache::EnableFullRetention());size_t resident=0;
    for(int i=0;i<1000;++i) {
      Book book;const auto path=root+"/cover"+std::to_string(i)+".png";
      book.SetBrowserFolderEntry(root,"folder",path);book.coverWidth=85;book.coverHeight=115;
      book.coverPixels=new u16[85*115];std::fill(book.coverPixels,book.coverPixels+85*115,(u16)i);
      assert(cover_cache::Save(&book,path));if(i==600) resident=live_bytes;
      if(i==999) assert(live_bytes<resident+16*1024 && "cover index RAM must stay bounded beyond512 files");
    }
  } else {
    // A fresh process must retain and load every cache, including the earliest.
    for(int i=0;i<1000;++i) {
      Book book;const auto path=root+"/cover"+std::to_string(i)+".png";
      book.SetBrowserFolderEntry(root,"folder",path);
      assert(cover_cache::TryLoadCachedOnly(&book,path) && book.coverPixels[0]==i);
    }
    unlink(paths::GetCoverRetentionPath().c_str());cover_cache::ResetMaintenance();
    // Normal retention resumes after clearing policy, including current-process state.
    Book book;assert(!cover_cache::TryLoadCachedOnly(&book,root+"/missing.png"));
    size_t files=0;DIR *dir=opendir(paths::GetCoverCacheDir().c_str());assert(dir);
    while(auto *entry=readdir(dir)) if(HasExtCI(entry->d_name,".cvr")) ++files;
    closedir(dir);assert(files<=512);
  }
  puts("PASS:1000 persistent covers exceed16MiB, bounded RAM, restart and reset retention");
}

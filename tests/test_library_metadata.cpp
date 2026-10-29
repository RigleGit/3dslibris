#include "formats/common/library_metadata.h"
#include "shared/path_constants.h"
#include <cassert>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
using namespace library_metadata;
static bool Cancel(void *) {return true;}
int main() {
  const std::string base=".";
  for(const char *name:{"sample.epub","sample.fb2","sample.mobi","sample.cbz","sample.xps"}) {
    const std::string path=base+"/"+name;
    Value a;assert(Prepare(path,&a)==Status::Ready && a.extracted && a.cache_saved);
    assert(a.entry.title=="El océano" && a.entry.author=="José Pérez");
    Value b;assert(Prepare(path,&b)==Status::Ready && !b.extracted);
    assert(b.entry.title==a.entry.title && b.entry.author==a.entry.author);
  }
  const std::string empty=base+"/empty.epub";
  struct stat st;assert(stat(empty.c_str(),&st)==0);
  book_meta_cache::MetaEntry legacy;legacy.title="empty.epub";
  const auto cache=book_meta_cache::BuildPath(empty,st.st_size,st.st_mtime);
  assert(book_meta_cache::Save(cache,legacy));
  Value v;assert(Prepare(empty,&v)==Status::Ready && v.extracted);
  assert(v.entry.title.empty() && v.entry.author.empty());
  bool prepared=false;assert(book_meta_cache::Load(cache,&legacy,&prepared) && prepared);
  assert(Prepare(empty,&v)==Status::Ready && !v.extracted);
  assert(Prepare(base+"/sample.txt",&v)==Status::Unsupported);
  assert(Prepare(base+"/sample.fb2",&v,Cancel,nullptr)==Status::Cancelled);
  for(const char *name:{"large-title.epub","large-xml.epub","bad.cbz","bad.mobi","bad.fb2","entity.fb2"})
    assert(Prepare(base+"/"+name,&v)==Status::Error);
  // Changed identity must force a new extraction without clearing caches.
  const auto path=base+"/sample.fb2";FILE *f=fopen(path.c_str(),"ab");assert(f);fputc(' ',f);fclose(f);
  assert(Prepare(path,&v)==Status::Ready && v.extracted);
  // A failed save still supplies genuine fields to this search.
  const auto missing=base+"/uncached.fb2";
  assert(stat(missing.c_str(),&st)==0);
  const auto blocked=book_meta_cache::BuildPath(missing,st.st_size,st.st_mtime);
  assert(mkdir(blocked.c_str(),0777)==0);
  assert(Prepare(missing,&v)==Status::Ready && v.extracted && !v.cache_saved);
  assert(v.entry.title=="El océano");
  puts("PASS: real metadata-only EPUB/FB2/MOBI/CBZ, cache migration, limits, cancel and write failure");
}

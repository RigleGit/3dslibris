#include "library/library_walk.h"
#include <cassert>
#include <vector>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
struct Source {std::vector<library_walk::Entry> roots;};
static bool Root(void *p,size_t i,library_walk::Entry *out) {
  auto &s=*static_cast<Source *>(p);if(i>=s.roots.size()) return false;*out=s.roots[i];return true;
}
int main() {
  char tmp[]="/tmp/library-walk-XXXXXX";const std::string root=mkdtemp(tmp),folder=root+"/García",deep=folder+"/deep";
  mkdir(folder.c_str(),0700);mkdir(deep.c_str(),0700);
  std::ofstream(deep+"/hidden.epub")<<"x";std::ofstream(folder+"/note.png")<<"x";
  for(int i=0;i<600;++i) std::ofstream(folder+"/book"+std::to_string(i)+".epub")<<"x";
  Source source{{{folder,"García",true}}};library_walk::Cursor cursor;
  assert(cursor.Start(Root,&source,true));size_t books=0,folders=0;
  library_walk::Entry entry;
  while(true) {
    const auto step=cursor.Next(&entry);if(step==library_walk::Step::End) break;
    if(step==library_walk::Step::Item) {if(entry.directory) ++folders;else ++books;assert(entry.path.find("deep")==std::string::npos);}
  }
  assert(books==600 && folders==1 && cursor.Errors()==0);
  assert(cursor.Start(Root,&source,false));assert(cursor.Next(&entry)==library_walk::Step::Item);
  assert(cursor.Next(&entry)==library_walk::Step::End);
  source.roots={{root+"/gone","gone",true}};assert(cursor.Start(Root,&source,true));
  assert(cursor.Next(&entry)==library_walk::Step::Item && cursor.Errors()==1);
  cursor.Close();assert(cursor.Next(&entry)==library_walk::Step::End);
  for(int i=0;i<600;++i) unlink((folder+"/book"+std::to_string(i)+".epub").c_str());
  unlink((deep+"/hidden.epub").c_str());unlink((folder+"/note.png").c_str());
  rmdir(deep.c_str());rmdir(folder.c_str());rmdir(root.c_str());
  puts("PASS: shared cursor scans600 books, one level, UTF8, no result cap and closes safely");
}

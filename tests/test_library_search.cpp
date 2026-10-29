#include "library/library_search.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
using namespace library_search;
struct Source { std::vector<Entry> roots; unsigned metadata_calls=0; };
static bool Root(void *ctx,size_t i,Entry *out) {
  Source &s=*(Source*)ctx;
  if(i>=s.roots.size()) return false;
  *out=s.roots[i]; return true;
}
static MetadataStatus Metadata(void *ctx,const std::string&,MetadataValue *out) {
  ++((Source*)ctx)->metadata_calls;
  out->title="Un título auténtico"; out->author="García";
  out->extracted=true; return MetadataStatus::Ready;
}
static void File(const std::string &path) { std::ofstream(path)<<"fixture"; }
int main() {
  char name[]="/tmp/library-search-XXXXXX"; const std::string dir=mkdtemp(name);
  const std::string child=dir+"/Comics",deep=child+"/deeper";
  mkdir(child.c_str(),0700); mkdir(deep.c_str(),0700);
  File(dir+"/García Álgebra.epub"); File(child+"/álgebra.pdf");
  File(deep+"/algebra.epub"); File(child+"/algebra.png");
  Source source; source.roots={{dir+"/García Álgebra.epub","García Álgebra.epub",false},
                               {child,"Comics",true}};
  Engine engine;
  assert(engine.Start(Field::Name,"ALGEBRA",Root,&source,true,Metadata));
  engine.Pump(1);
  assert(engine.Examined()==1 && engine.Results().size()==1);
  while(engine.GetStatus()==Status::Searching) engine.Pump(1);
  assert(engine.Results().size()==2 && source.metadata_calls==0);
  for(const auto &r:engine.Results()) assert(r.path.find("deeper")==std::string::npos);
  assert(engine.Start(Field::Name,"comic",Root,&source,true,Metadata));
  while(engine.GetStatus()==Status::Searching) engine.Pump();
  assert(engine.Results().size()==1 && engine.Results()[0].directory);
  assert(engine.Start(Field::Name,"garcia algebra",Root,&source,false,Metadata));
  while(engine.GetStatus()==Status::Searching) engine.Pump();
  assert(engine.Results().size()==1);
  assert(engine.Start(Field::Title,"autentico",Root,&source,true,Metadata));
  engine.Pump();
  assert(source.metadata_calls==1 && engine.GetStatus()==Status::Searching);
  while(engine.GetStatus()==Status::Searching) engine.Pump();
  assert(engine.Results().size()==2 && source.metadata_calls==2);
  assert(engine.Start(Field::Author,"GARCIA",Root,&source,true,Metadata));
  engine.Cancel(); assert(engine.GetStatus()==Status::Cancelled);
  assert(!engine.Start(Field::Name,"  ",Root,&source,true,Metadata));
  source.roots.clear();
  for(int i=0;i<300;++i) source.roots.push_back({dir+"/book"+std::to_string(i)+".epub","book.epub",false});
  assert(engine.Start(Field::Name,"book",Root,&source,false,Metadata));
  while(engine.GetStatus()==Status::Searching) engine.Pump();
  assert(engine.Results().size()==256 && engine.GetStatus()==Status::Limited);
  assert(engine.AuxiliaryBytes()<=256u*1024u);
  for(const auto &e:source.roots) (void)e;
  unlink((dir+"/García Álgebra.epub").c_str()); unlink((child+"/álgebra.pdf").c_str());
  unlink((deep+"/algebra.epub").c_str()); unlink((child+"/algebra.png").c_str());
  rmdir(deep.c_str()); rmdir(child.c_str()); rmdir(dir.c_str());
  puts("PASS: one-level search, Unicode AND matching, metadata isolation, budgets and cancellation");
}

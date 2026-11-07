#include "library/library_preload.h"
#include <cassert>
#include <cstdio>
using namespace library_preload;
struct Source {
  unsigned metadata=0,covers=0,heavy=0;
  int write_error=-1,read_error=-1;
};
static bool Root(void *,size_t i,library_walk::Entry *out) {
  if(i>=600) return false;
  *out={"/library/book"+std::to_string(i)+".epub","book.epub",false};return true;
}
static Result Metadata(void *ctx,const library_walk::Entry &) {
  auto &s=*(Source*)ctx;const int i=s.metadata++;++s.heavy;
  return {i==s.read_error ? Outcome::Error : Outcome::Saved,true};
}
static Result Cover(void *ctx,const library_walk::Entry &) {
  auto &s=*(Source*)ctx;const int i=s.covers++;++s.heavy;
  return {i==s.write_error ? Outcome::WriteError : Outcome::Saved,true};
}
static Result Cached(void *,const library_walk::Entry &) {return {Outcome::Cached,false};}
static uint64_t ticks=0;
static uint64_t Clock() {return ticks++;}
int main() {
  Engine engine;Source s;
  assert(engine.Start(Root,&s,Metadata,Cover));
  engine.Pump();assert(s.heavy==1 && s.metadata==1 && s.covers==0);
  engine.Pump();assert(s.heavy==2 && s.covers==1);
  while(engine.GetStatus()==Status::Running) {const auto before=s.heavy;engine.Pump();assert(s.heavy<=before+1);}
  assert(engine.GetStatus()==Status::Complete && engine.Examined()==600);
  assert(engine.MetadataCounts().saved==600 && engine.CoverCounts().saved==600);
  s=Source();s.read_error=0;s.write_error=2;
  assert(engine.Start(Root,&s,Metadata,Cover));
  while(engine.GetStatus()==Status::Running) engine.Pump();
  assert(engine.GetStatus()==Status::WriteError && s.covers==3 && s.metadata==3);
  assert(engine.MetadataCounts().errors==1 && engine.CoverCounts().errors==1);
  assert(engine.Start(Root,&s,Metadata,Cover));engine.Cancel();
  const auto before=s.heavy;engine.Pump();assert(s.heavy==before && engine.GetStatus()==Status::Cancelled);
  s=Source();assert(engine.Start(Root,&s,Metadata,Cover));ticks=0;engine.Pump(16,Clock,0);assert(s.heavy==0);
  assert(engine.Start(Root,&s,Cached,Cached));engine.Pump(16);
  assert(engine.Examined()==16 && engine.MetadataCounts().cached==16 && engine.CoverCounts().cached==16);
  assert(engine.Start(Root,&s,Cached,Cached));ticks=0;engine.Pump(16,Clock,4);
  assert(engine.Examined()==1 && engine.MetadataCounts().cached==1 && engine.CoverCounts().cached==1);
  puts("PASS:600book preload, oneheavyoperation/update, independent stages, cancellation and stoponwritefailure");
}

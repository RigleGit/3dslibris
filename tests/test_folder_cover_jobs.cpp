#include "test_folder_cover_support.h"

u64 test_now=10000, test_free_bytes=64u*1024u*1024u;
size_t test_source_reads=0, test_decodes=0;
static size_t largest_decoder_allocation=0;
void *CoverTestMalloc(size_t bytes) {
  largest_decoder_allocation=std::max(largest_decoder_allocation,bytes);
  return std::malloc(bytes);
}
void *CoverTestRealloc(void *pointer,size_t bytes) {
  largest_decoder_allocation=std::max(largest_decoder_allocation,bytes);
  return std::realloc(pointer,bytes);
}
void CoverTestFree(void *pointer) { std::free(pointer); }
namespace file_read_utils {
bool CountedReadPathToStringLimited(const std::string &path,std::string *out,size_t limit) {
  ++test_source_reads;
  return ReadPathToStringLimited(path,out,limit);
}
}
namespace stb_image_gif_utils {
unsigned char *CountedLoadFromMemory(const unsigned char *bytes,int length,int *w,int *h,int *channels,int requested) {
  ++test_decodes;
  return LoadFromMemory(bytes,length,w,h,channels,requested);
}
}
static void Expect(bool condition,const char *message) {
  if (!condition) { fprintf(stderr,"FAIL: %s (source_reads=%zu decodes=%zu)\n",message,test_source_reads,test_decodes); std::exit(1); }
}
static size_t RetainedBytes(const App &app) {
  size_t bytes=0;
  for (const Book *book:app.books) if(book->coverPixels) bytes+=(size_t)book->coverWidth*book->coverHeight*sizeof(u16);
  return bytes;
}
static void Write(const std::string &path,const std::string &bytes) {
  FILE *fp=fopen(path.c_str(),"wb"); Expect(fp!=nullptr,"fixture opened");
  Expect(fwrite(bytes.data(),1,bytes.size(),fp)==bytes.size(),"fixture written"); fclose(fp);
}
static std::string Read(const std::string &path) {
  std::string bytes; Expect(file_read_utils::ReadPathToStringLimited(path,&bytes,4u*1024u*1024u),"fixture read"); return bytes;
}
static int CheckOversizedCoverPreflight() {
  Book book;
  book.coverPixels=new u16[1]; book.coverPixels[0]=0x1234;
  book.coverWidth=book.coverHeight=1;
  u16 *previous=book.coverPixels;
  const std::string bytes=Read(std::getenv("FOLDER_COVER_OVERSIZED_PNG"));
  largest_decoder_allocation=0;
  const bool ok=cover_decode_utils::DecodeImageToCoverThumb(
      &book,reinterpret_cast<const unsigned char *>(bytes.data()),bytes.size(),4096);
  Expect(!ok,"out-of-policy cover rejected");
  printf("oversized preflight: largest_decoder_allocation=%zu previous_cover=%04x\n",
         largest_decoder_allocation,book.coverPixels[0]);
  Expect(largest_decoder_allocation<128u*1024u,"oversized cover must not allocate its expanded pixels");
  Expect(book.coverPixels==previous && book.coverPixels[0]==0x1234 &&
         book.coverWidth==1 && book.coverHeight==1,"rejected cover preserves previous thumbnail");
  return 0;
}
int main() {
  if(std::getenv("FOLDER_COVER_PREFLIGHT")) return CheckOversizedCoverPreflight();
  const std::string root=std::getenv("FOLDER_COVER_TEST_ROOT");
  const std::string png=Read(std::getenv("FOLDER_COVER_PNG"));
  App app; LibraryController controller(app); std::set<std::string> seen;
  for(int i=0;i<128;++i) {
    const std::string name="folder-"+std::to_string(i), dir=root+"/"+name;
    mkdir(dir.c_str(),0777); Write(dir+"/book.epub","fixture"); Write(root+"/"+name+".png",png);
    AppendTestFolder(&app,root,name,&seen);
  }
  Expect(app.BookCount()==128,"all folders indexed"); app.selected=app.books[0];
  printf("indexed: folders=128 reads=%zu decodes=%zu retained=%zu\n",test_source_reads,test_decodes,RetainedBytes(app));
  if(std::getenv("FOLDER_COVER_BASELINE")) {
    for(Book *book:app.books) delete book;
    return 0;
  }
  Expect(test_source_reads==0 && test_decodes==0 && RetainedBytes(app)==0,"indexing must not read/decode/retain sidecars");
  controller.LoadVisibleBrowserCoverCaches();
  Expect(test_source_reads==0 && test_decodes==0,"first page cache misses must not decode during input");
  controller.TickBrowserWarmup();
  Expect(!controller.job_queue_.empty(),"visible folders queued for idle extraction");
  app.page_start=4; app.selected=app.books[4]; controller.LoadVisibleBrowserCoverCaches();
  controller.ProcessJobs(3);
  Expect(test_decodes==0 && controller.job_queue_.empty(),"stale first-page jobs discarded before source read");
  // Keep platform admission real: held input and low-memory Old3DS must defer.
  app.wait_release=true; controller.TickBrowserWarmup(); Expect(controller.job_queue_.empty(),"held input defers warmup");
  app.wait_release=false; app.is_new=false; test_free_bytes=1;
  controller.TickBrowserWarmup(); controller.ProcessJobs(3);
  Expect(test_decodes==0,"low-memory cover admission preserved");
  app.is_new=true; test_free_bytes=64u*1024u*1024u; test_now+=10000;
  for(int i=0;i<4;++i) { controller.TickBrowserWarmup(); const size_t before=test_decodes; controller.ProcessJobs(3); Expect(test_decodes==before+1,"one actual decode per idle pump"); }
  Expect(test_source_reads==4 && test_decodes==4,"only visible page sidecars read/decoded");
  Expect(RetainedBytes(app)==4u*85u*115u*2u,"visible covers retain 78200 bytes");
  const u16 original=app.books[4]->coverPixels[0];
  printf("idle page: reads=%zu decodes=%zu retained=%zu\n",test_source_reads,test_decodes,RetainedBytes(app));
  app.page_start=8; app.selected=app.books[8]; controller.LoadVisibleBrowserCoverCaches();
  Expect(RetainedBytes(app)==0 && test_decodes==4,"page change evicts covers without decoding");
  controller.TickBrowserWarmup();
  app.prefs->browser_view_mode=BROWSER_VIEW_LIST;
  controller.LoadVisibleBrowserCoverCaches(); controller.ProcessJobs(3);
  Expect(RetainedBytes(app)==0 && test_decodes==4 && controller.job_queue_.empty(),"list mode discards pending folder covers");
  app.prefs->browser_view_mode=BROWSER_VIEW_GALLERY; app.page_start=4; app.selected=app.books[4];
  controller.LoadVisibleBrowserCoverCaches();
  Expect(test_source_reads==4 && test_decodes==4 && RetainedBytes(app)==78200,"revisit reloads thumbnail cache without sidecar decode");
  Expect(app.books[4]->coverPixels[0]==original,"cache preserves decoded pixels");
  controller.TickBrowserWarmup(); Expect(controller.job_queue_.empty(),"loaded covers not requeued");
  // Same file path, changed sidecar identity must invalidate the .cvr entry.
  app.prefs->browser_view_mode=BROWSER_VIEW_LIST; controller.LoadVisibleBrowserCoverCaches();
  Write(root+"/folder-4.png",png+"changed");
  app.prefs->browser_view_mode=BROWSER_VIEW_GALLERY; controller.LoadVisibleBrowserCoverCaches();
  Expect(!app.books[4]->coverPixels && app.books[5]->coverPixels,"changed sidecar invalidates only its thumbnail");
  controller.TickBrowserWarmup(); controller.ProcessJobs(3);
  Expect(test_source_reads==5 && test_decodes==5 && app.books[4]->coverPixels,"changed sidecar regenerated at idle");
  printf("revisit + invalidation: reads=%zu decodes=%zu retained=%zu\n",test_source_reads,test_decodes,RetainedBytes(app));
  // A broken sidecar should not be repeatedly decoded while visible, but
  // returning to the page must allow a repaired file to be tried again.
  Write(root+"/folder-0.png","invalid PNG");
  app.page_start=0; app.selected=app.books[0]; controller.LoadVisibleBrowserCoverCaches();
  controller.TickBrowserWarmup(); controller.ProcessJobs(3);
  Expect(!app.books[0]->coverPixels && app.books[0]->coverAttempts==3,"broken sidecar exhausted once");
  controller.TickBrowserWarmup();
  for (const app_job_t &job:controller.job_queue_) Expect(job.book!=app.books[0],"broken visible sidecar not retried every frame");
  controller.PauseBrowserJobs();
  Write(root+"/folder-0.png",png);
  app.page_start=4; app.selected=app.books[4]; controller.LoadVisibleBrowserCoverCaches();
  app.page_start=0; app.selected=app.books[0]; controller.LoadVisibleBrowserCoverCaches();
  controller.TickBrowserWarmup(); controller.ProcessJobs(3);
  Expect(app.books[0]->coverPixels!=nullptr,"revisit permits repaired sidecar extraction");
  // Coverless directories must not create useless decode jobs.
  controller.PauseBrowserJobs();
  const std::string bare=root+"/bare-folder";
  mkdir(bare.c_str(),0777); Write(bare+"/book.epub","fixture");
  AppendTestFolder(&app,root,"bare-folder",&seen);
  app.page_start=128; app.selected=app.books[128]; controller.LoadVisibleBrowserCoverCaches();
  const size_t before_bare=test_decodes;
  controller.TickBrowserWarmup(); controller.ProcessJobs(3);
  Expect(test_decodes==before_bare && controller.job_queue_.empty() && RetainedBytes(app)==0,"coverless folder does not allocate or queue decode");
  for(Book *book:app.books) delete book;
  puts("folder cover integration passed");
}

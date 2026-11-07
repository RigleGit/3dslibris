#include "app/app_mode.h"
#include "app/frame_input.h"
#include "shared/app_flow_utils.h"
#include "formats/common/library_metadata.h"
#include "library/cover_preparation.h"
#include "library/browser_view_utils.h"
#include "library/library_sort_mode.h"
#include "library/library_preload.h"
#include <algorithm>
#include <sys/stat.h>
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>
#include <new>
#define DBG_LOGF(...) ((void)0)
static unsigned live_books=0,max_books=0,metadata_calls=0,cover_calls=0,cache_reads=0,cover_reads=0;
static bool write_ok=true,retention_ok=true,metadata_cached=false,cover_cached=false;
static unsigned presentations=0;
static library_preload::Phase visible_phase;
struct BookContext {void *text=nullptr,*prefs=nullptr,*status_reporter=nullptr;};
struct Book {
  std::string path,name,coverImagePath;bool directory=false,metadataIndexTried=false,metadataIndexed=false;
  format_t format=FORMAT_UNDEF;
  bool tracked;u16 *coverPixels=nullptr;int coverWidth=0,coverHeight=0,coverAttempts=0;u64 coverRetryAfterMs=0;
  Book(const BookContext &,bool track=true):tracked(track) {if(tracked){++live_books;if(live_books>max_books)max_books=live_books;}}
  ~Book() {if(tracked)--live_books;delete[] coverPixels;}
  bool TryLoadMetadataFromCache() {++cache_reads;return true;}
  void ClearBrowserDisplayNameCache() {}
  void SetFolderName(const std::string &s) {path=s;}
  void SetFileName(const char *s) {name=s;}
  void SetBrowserFolderEntry(const std::string &p,const std::string &n,const std::string &) {path=p;name=n;directory=true;}
  bool IsBrowserFolder() const {return directory;}
  const std::string &GetBrowserFolderPath() const {return path;}
  const std::string &GetBrowserFolderDisplayName() const {return name;}
  std::string GetBrowserFolderCoverPath() const {return path+".png";}
  const char *GetFileName() const {return name.c_str();}
};
#include "library/library_preload_session.h"
static Text button_text;
Button::Button():ts(&button_text) {}
void Button::Label(const char *label) {text1=label;}
void Button::Draw(u16 *,bool) {}
bool Button::EnclosesPoint(u16,u16) {return false;}
static uint64_t PreloadClock() {return 0;}
static std::string PreloadPath(Book *b) {return b->directory ? b->path : b->path+"/"+b->name;}
namespace cover_cache {bool EnableFullRetention() {return retention_ok;}
bool TryLoadCachedOnly(Book *,const std::string &) {++cover_reads;return false;}
}
namespace library_metadata {
Status Prepare(const std::string &path,Value *out,Cancelled cancel,void *ctx,bool cache_only) {
 if(cache_only) {out->extracted=false;return metadata_cached ? Status::Ready : Status::NeedsPreparation;}
 ++metadata_calls;assert(live_books==1);
 assert(presentations>0 && visible_phase==library_preload::Phase::Metadata && "metadata must start after presentation");
 if(cancel(ctx))return Status::Cancelled;
 out->extracted=true;out->cache_saved=write_ok;
 out->entry.cover_image_path="image.png";
 return path.find("bad")!=std::string::npos ? Status::Error : Status::Ready;
}
}
namespace library_cover {
Result Prepare(Book *book,const std::string &path,bool,bool,bool cache_only) {
 if(cache_only) return {cover_cached ? Status::Cached : Status::NeedsPreparation,0,false};
 assert(presentations>0 && (book->directory || visible_phase==library_preload::Phase::Cover) && "cover must start after presentation");
 ++cover_calls;assert(live_books==1);
 assert(book->metadataIndexTried || book->directory);
 if(book->directory) assert(path==book->GetBrowserFolderCoverPath() && "folder extraction receives associated image, not directory");
 return {Status::Prepared,0,true};
}
}
struct Prefs {BrowserViewMode browser_view_mode=BROWSER_VIEW_GALLERY;};
struct App {
 std::vector<Book*> books;std::unique_ptr<Text> ts{new Text};std::unique_ptr<Prefs> prefs{new Prefs};
 Book *selected=nullptr;int page=0;unsigned settings_shown=0;
 Book *GetSelectedBook() {return selected;}
 void SetSelectedBook(Book *b) {selected=b;}
 int GetBookIndex(Book *b) {for(size_t i=0;i<books.size();++i)if(books[i]==b)return i;return -1;}
 int BookCount() {return books.size();}
 int GetBrowserPageStart() {return page;}
 void SetBrowserPageStart(int p) {page=p;}
 void SetBrowserDirty(bool) {}
 struct {u32 a=1,b=2;}key;u8 portrait_orientation=0;
 bool abort=false;AppMode mode=AppMode::LibraryPreload;
 bool ShouldAbortWork() {return abort;}
 AppMode GetMode() {return mode;}
 bool IsNew3dsDevice() {return true;}
 void ShowSettingsView(bool) {++settings_shown;mode=AppMode::Prefs;}
 void ApplyRenderOrientation(u8) {}
 void PresentIfDirty() {
   ++presentations;
   for(auto i=ts->printed.rbegin();i!=ts->printed.rend();++i) {
     if(i->screen!=ts->screenleft) continue;
     if(i->text=="metadata") {visible_phase=library_preload::Phase::Metadata;break;}
     if(i->text=="cover") {visible_phase=library_preload::Phase::Cover;break;}
   }
 }
};
enum {KEY_TOUCH=4};
struct LibraryController {
 App &app_;bool inside_folder_=true,root_cached_=true;
 std::vector<Book*>root_books_;
 std::string current_folder_path_="/tmp";
 LibrarySortMode root_sort_mode_=LIBRARY_SORT_TITLE;
 std::unique_ptr<LibraryPreloadSession>preload_{new LibraryPreloadSession};
 unsigned draws=0,closed=0;
 LibraryController(App &a):app_(a){}
 static bool PreloadRoot(void*,size_t,library_walk::Entry*);
 static bool PreloadCancelled(void*);
 Book *PreloadBook(const library_walk::Entry&);
 static library_preload::Result PreloadMetadata(void*,const library_walk::Entry&);
 static library_preload::Result PreloadCover(void*,const library_walk::Entry&);
 void DrawLibraryPreload();
 void ResumeLibraryPreload();void RunLibraryPreloadFrame(const FrameInput&);
 void CloseLibraryPreload();
 void SortBooks(){std::reverse(app_.books.begin(),app_.books.end());}
 void ResetBrowserMarquee() {}
 void RebuildRoot() {inside_folder_=false;app_.books=root_books_;app_.selected=app_.books[0];}
};
static std::string FitPreload(Text *,const std::string &text) {return text;}
#include "preload_ui.inc"
int main() {
 App app;LibraryController owner(app);FrameInput input;
 // Root book objects stand in for the retained browser inventory, excluded from temporary accounting.
 Book root{BookContext(),false},child{BookContext(),false};root.path="/library";root.name="bad.epub";
 child.path="/library/folder";child.name="visible.epub";
 live_books=max_books=0;owner.root_books_={&root};app.books={&child};
 library_walk::Entry e;assert(owner.PreloadRoot(&owner,0,&e) && e.path=="/library/bad.epub");
 owner.RunLibraryPreloadFrame(input);assert(metadata_calls==0 && cover_calls==0);
 owner.RunLibraryPreloadFrame(input);assert(metadata_calls==0 && cover_calls==0); // initialization has its own frame
 owner.RunLibraryPreloadFrame(input);assert(metadata_calls==1 && cover_calls==0);
 owner.RunLibraryPreloadFrame(input);assert(cover_calls==1 && live_books==0); // metadata failure still permits cover
 input.keys_down=app.key.b;owner.RunLibraryPreloadFrame(input);
 assert(owner.preload_->finished && owner.preload_->engine.GetStatus()==library_preload::Status::Cancelled);
 input.keys_down=0;owner.ResumeLibraryPreload();assert(owner.preload_->start_pending && !owner.preload_->finished);
 owner.RunLibraryPreloadFrame(input);owner.RunLibraryPreloadFrame(input);
 write_ok=false;root.name="good.epub";
 owner.RunLibraryPreloadFrame(input);
 assert(owner.preload_->finished && owner.preload_->engine.GetStatus()==library_preload::Status::WriteError);
 assert(cover_calls==1 && live_books==0 && max_books==1);
 // Failed marker cannot start extraction; summary returns to preferences.
 owner.ResumeLibraryPreload();owner.RunLibraryPreloadFrame(input);retention_ok=false;
 unsigned before=metadata_calls;owner.RunLibraryPreloadFrame(input);
 assert(owner.preload_->finished && metadata_calls==before && !owner.preload_->message.empty());
 input.keys_down=app.key.b;owner.RunLibraryPreloadFrame(input);
 assert(app.settings_shown==1 && app.mode==AppMode::Prefs);
 owner.preload_.reset(new LibraryPreloadSession);app.mode=AppMode::LibraryPreload;
 // Metadata cache hits must still present the next heavy cover before decoding.
 retention_ok=true;metadata_cached=true;owner.RunLibraryPreloadFrame(FrameInput());owner.RunLibraryPreloadFrame(FrameInput());
 const auto metadata_before=metadata_calls;owner.RunLibraryPreloadFrame(FrameInput());
 assert(metadata_calls==metadata_before && cover_calls==2);
 cover_cached=true;owner.ResumeLibraryPreload();owner.RunLibraryPreloadFrame(FrameInput());owner.RunLibraryPreloadFrame(FrameInput());
 const auto shown=presentations;owner.RunLibraryPreloadFrame(FrameInput());
 assert(owner.preload_->finished && presentations==shown && owner.preload_->engine.MetadataCounts().cached==1 && owner.preload_->engine.CoverCounts().cached==1);
 app.ts->printed.clear();owner.DrawLibraryPreload();
 bool complete=false,stale=false,instructions=false;
 for(const auto &line:app.ts->printed) {
   complete|=line.screen==app.ts->screenleft && line.text=="PRELOAD COMPLETE";
   stale|=line.screen==app.ts->screenleft && line.text=="scanning library";
   instructions|=line.text=="A / B: return to settings";
 }
 assert(complete && instructions && !stale && "completed preload must visibly replace the active progress view");
 assert(std::string(owner.preload_->action.GetLabel())=="back to settings");
 cover_cached=false;
 owner.preload_.reset(new LibraryPreloadSession);app.mode=AppMode::LibraryPreload;
 visible_phase=library_preload::Phase::Cover;
 auto folder=library_walk::Entry{"/library/folder", "folder", true};
 const auto prepared=owner.PreloadCover(&owner,folder);
 assert(prepared.outcome==library_preload::Outcome::Saved && live_books==0);
 // A cache-only refresh may reorder books without losing the selected entry.
 std::unique_ptr<Book> inventory[7];app.books={&child};app.selected=&child;
 for(auto &book:inventory) {book.reset(new Book(BookContext(),false));app.books.push_back(book.get());}
 cache_reads=cover_reads=0;owner.CloseLibraryPreload();
 assert(app.selected==&child && app.page==4 && app.GetBookIndex(&child)==7);
 assert(cache_reads==9 && cover_reads==4 && !owner.preload_);
 owner.preload_.reset(new LibraryPreloadSession);app.mode=AppMode::LibraryPreload;owner.current_folder_path_="/missing-preload-folder";
 owner.CloseLibraryPreload();assert(!owner.inside_folder_ && app.books.size()==1 && app.selected==&root);
 puts("PASS: retained root traversal, staged UI, one temporary book, metadata failure, write stop, cancel and HOME restart");
}

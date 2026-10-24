#pragma once
#include "shared/fixed_layout_perf.h"
// Platform/UI substitutes only. Directory I/O, job admission, disk cache,
// thumbnail decode and resampling below are production implementations.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "shared/app_flow_utils.h"
#include "shared/string_utils.h"
#include "shared/debug_runtime_mode.h"
#include "shared/stb_image_gif_utils.h"
#include "shared/cover_decode_utils.h"
#include "shared/aspect_fit_utils.h"
#include "shared/image_scale_utils.h"
#include "formats/common/file_read_utils.h"
#include "formats/common/book_error.h"
#include "library/browser_cover_cache_utils.h"
#include "library/browser_job_queue_utils.h"
#include "library/browser_view_utils.h"
#include "library/browser_warmup_utils.h"
#include "library/cover_override_utils.h"
#include "library/cover_cache.h"
#include "library/library_job.h"
#include "book/cover_layout_constants.h"
#include "stb_image.h"
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
extern u64 test_now, test_free_bytes;
inline u64 osGetTime() { return test_now; }
static const int MEMREGION_ALL=0;
inline u64 osGetMemRegionFree(int) { return test_free_bytes; }
struct Text { void *screenright=nullptr; void MarkScreenDirty(void *) {} };
struct Prefs { bool disable_ebook_images=false; BrowserViewMode browser_view_mode=BROWSER_VIEW_GALLERY; };
struct BookContext {
  Text *text=nullptr; Prefs *prefs=nullptr; bool *disable_ebook_images=nullptr;
  int *paragraph_spacing=nullptr, *paragraph_indent=nullptr;
  bool *publisher_text_indent=nullptr, *publisher_block_margins=nullptr, *publisher_horizontal_margins=nullptr;
  int *orientation=nullptr; void *status_reporter=nullptr;
  void (*draw_background)(void *)=nullptr; void *draw_background_user_data=nullptr;
  void (*draw_top_background)(void *)=nullptr; void *draw_top_background_user_data=nullptr;
  void (*on_spine_progress)(unsigned,unsigned,void *)=nullptr; void *on_spine_progress_user_data=nullptr;
};
class Book {
  bool is_folder_=false; std::string folder_, name_, sidecar_;
public:
  explicit Book(const BookContext & = BookContext()) {}
  ~Book() { delete[] coverPixels; }
  format_t format=FORMAT_UNDEF; bool metadataIndexTried=false, tocResolveTried=false, tocResolved=false;
  u16 *coverPixels=nullptr; int coverWidth=0, coverHeight=0; u8 coverAttempts=0;
  u64 coverRetryAfterMs=0; std::string coverImagePath;
  bool IsBrowserFolder() const { return is_folder_; }
  void SetBrowserFolderEntry(const std::string &path,const std::string &name,const std::string &sidecar) { is_folder_=true; folder_=path; name_=name; sidecar_=sidecar; }
  const std::string &GetBrowserFolderCoverPath() const { return sidecar_; }
  const char *GetFolderName() const { return folder_.c_str(); }
  const char *GetFileName() const { return is_folder_ ? "" : name_.c_str(); }
  const char *GetTitle() const { return name_.c_str(); }
  void *GetStatusReporter() const { return nullptr; }
  void ClearBrowserDisplayNameCache() {}
};
enum class AppMode { Browser, Chapters, Book };
struct ChapterMenu { void Init() {} void SetDirty(bool) {} };
class App {
public:
  std::unique_ptr<Text> ts{new Text}; std::unique_ptr<Prefs> prefs{new Prefs};
  std::vector<Book *> books; Book *selected=nullptr; int page_start=0;
  bool is_new=true, suspended=false, wait_release=false, dirty=false;
  u64 last_interaction=0; AppMode mode=AppMode::Browser;
  int paraspacing=0, paraindent=0, orientation=0;
  bool publisher_text_indent=false, publisher_block_margins=false, publisher_horizontal_margins=false;
  ChapterMenu *chaptermenu=nullptr;
  int BookCount() const { return (int)books.size(); }
  int GetBrowserPageStart() const { return page_start; }
  Book *GetSelectedBook() const { return selected; }
  Book *GetCurrentBook() const { return nullptr; }
  AppMode GetMode() const { return mode; }
  bool IsAppletSuspended() const { return suspended; }
  bool ShouldAbortWork() const { return suspended; }
  bool IsNew3dsDevice() const { return is_new; }
  bool IsBrowserWaitingInputRelease() const { return wait_release; }
  u64 GetBrowserLastInteractionMs() const { return last_interaction; }
  void SetBrowserDirty(bool value) { dirty=value; }
  int GetBookIndex(Book *book) const { for (size_t i=0;i<books.size();++i) if(books[i]==book) return (int)i; return -1; }
};
struct LibraryGradientContext { Text *ts; const u8 *color_mode; };
class LibraryController {
public:
  App &app_; std::deque<app_job_t> job_queue_;
  explicit LibraryController(App &app): app_(app) {}
  void UnloadNonVisibleBrowserCoverCaches(); void LoadVisibleBrowserCoverCaches();
  void PrioritizeSelectedBookJobs(Book *); bool HasQueuedJob(app_job_type_t,Book *) const;
  void EnqueueJob(app_job_type_t,Book *); void QueueBookWarmup(Book *); void TickBrowserWarmup();
  void QueueTocResolve(Book *); void ProcessJobs(u32); size_t PauseBrowserJobs();
  void ResetBrowserMarquee() {}
};
struct HomeButtonGuard {};
inline void DrawBottomGradientFromApp(void *) {}
inline void DrawTopGradientFromApp(void *) {}
inline void DrawOpeningSplashWithProgress(unsigned,unsigned,void *) {}
namespace book_parser { inline int Index(Book *) { assert(false && "Unexpected book indexing"); return 1; } }
#define TEST_UNUSED_FORMAT(name) namespace name { inline int ExtractCover(Book *,const std::string &) { assert(false && "Unexpected book cover parser"); return 1; } }
TEST_UNUSED_FORMAT(epub_parser) TEST_UNUSED_FORMAT(fb2_parser) TEST_UNUSED_FORMAT(mobi_parser) TEST_UNUSED_FORMAT(pdf_parser) TEST_UNUSED_FORMAT(cbz_parser)
namespace epub_parser { inline int ResolveToc(Book *,const std::string &) { assert(false); return 1; } }
namespace app_flow_utils {
inline bool ShouldIndexBookFilename(const char *name) { return HasExtCI(name,".epub"); }
inline format_t DetectBookFormat(const char *name) { return HasExtCI(name,".epub") ? FORMAT_EPUB : FORMAT_UNDEF; }
inline bool SupportsMetadataIndexing(format_t type) { return type==FORMAT_EPUB; }
}
namespace paths {
inline const std::string &GetCacheBaseDir() { static const std::string path=std::string(std::getenv("FOLDER_COVER_TEST_ROOT"))+"/cache"; return path; }
inline const std::string &GetCoverCacheDir() { static const std::string path=GetCacheBaseDir()+"/cover"; return path; }
inline const std::string &GetCoverCacheManifest() { static const std::string path=GetCacheBaseDir()+"/manifest.txt"; return path; }
}
inline std::string BasenamePath(const std::string &path) { const size_t slash=path.find_last_of('/'); return slash==std::string::npos ? path : path.substr(slash+1); }
extern size_t test_source_reads, test_decodes;
namespace file_read_utils {
bool CountedReadPathToStringLimited(const std::string &,std::string *,size_t);
}
namespace stb_image_gif_utils {
unsigned char *CountedLoadFromMemory(const unsigned char *,int,int *,int *,int *,int);
}
void AppendTestFolder(App *,const std::string &,const std::string &,std::set<std::string> *);

/*
    3dslibris - book.h
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Book domain model (metadata, pages, chapters, bookmarks, parse context).
    - Tracks source path/format and reading progress for persistence.
    - Adds 3DS-side helpers for browser labels and inline image cache
   bookkeeping.
*/

#pragma once

#include "book/book_context.h"
#include "book/inline_image_layout.h"
#include "shared/app_flow_utils.h"
#include <3ds.h>
#include <list>
#include <memory>
#include <stddef.h>
#include <string>
#include <unordered_map>
#include <vector>

class IStatusReporter;
class Page;
class Text;
class Prefs;
struct CbzPageEntry;
struct fz_context;
struct fz_document;
struct fz_display_list;
struct fz_outline;

struct ChapterEntry {
  u16 page; // page index where chapter starts
  u8 level; // toc nesting depth (0 = top-level)
  std::string title;
};

enum TocQuality {
  TOC_QUALITY_UNKNOWN = 0,
  TOC_QUALITY_STRONG,
  TOC_QUALITY_MIXED,
  TOC_QUALITY_HEURISTIC
};

//! Encapsulates metadata and Page vector for a single book.

//! Bookmarks are in here too.
//! App maintains a vector of Book to represent the available library.

class Book {
public:
  struct MuPdfState;
  struct CbzState;
  struct ReflowWorkerState;
  struct InlineImageEntry {
    std::string path;
    bool metadata_probed;
    bool metadata_ok;
    int source_width;
    int source_height;
    u8 follow_text_lines;

    InlineImageEntry()
        : metadata_probed(false), metadata_ok(false), source_width(0),
          source_height(0), follow_text_lines(0) {}
  };

  struct InlineImageCacheEntry {
    u16 image_id;
    u16 screen_h;
    u16 bg565;
    u8 layout_mode;
    u16 width;
    u16 height;
    std::vector<u16> pixels;
  };

private:
  std::string filename;
  std::string foldername;
  std::string title;
  std::string author;
  std::string browser_display_name_cache;
  bool browser_display_name_cached;
  int position;             //! as page index.
  std::list<u16> bookmarks; //! as page indices.
  std::vector<ChapterEntry> chapters;
  std::vector<InlineImageEntry> inline_images;
  std::unordered_map<std::string, u16> inline_image_path_index;
  std::unordered_map<std::string, u16> chapter_anchor_pages;
  std::unordered_map<std::string, u16> chapter_doc_start_pages;
  std::unordered_map<std::string, std::vector<u8>> fb2_inline_images;
  void *inline_image_probe_uf;
  bool inline_image_zip_index_built;
  std::unordered_map<std::string, unsigned long> inline_image_zip_offsets;
  bool mobi_inline_index_ready;
  u32 mobi_first_image_index;
  std::vector<u32> mobi_record_offsets;
  size_t fb2_inline_images_bytes;
  std::list<InlineImageCacheEntry> inline_image_cache;
  std::unordered_map<u64, std::list<InlineImageCacheEntry>::iterator> inline_image_cache_index;
  size_t inline_image_cache_bytes;
  TocQuality toc_quality;
  u16 toc_direct_count;
  u16 toc_heuristic_count;
  u16 toc_unresolved_count;
  std::vector<Page *> pages;
  MuPdfState *mupdf_state;
  CbzState *cbz_state;
  ReflowWorkerState *reflow_worker_state;
  BookContext ctx;
  unsigned int layout_revision;

  void ClearInlineImageCache();
  bool LoadInlineImageSource(u16 image_id, std::vector<u8> *out,
                             std::string *resolved_path = NULL);
  bool EnsureInlineImageMetadata(u16 image_id, InlineImageMetadata *out);
  void ResetMuPdfState();
  void ResetCbzState();
  void ResetReflowWorkerState();

public:
  //! Cover thumbnail for library grid (RGB565, scaled to fit)
  u16 *coverPixels;
  int coverWidth;
  int coverHeight;
  std::string coverImagePath; //! path inside EPUB zip
  bool coverTried;
  bool metadataIndexTried;
  bool metadataIndexed;
  bool tocResolveTried;
  bool tocResolved;
  bool epub_page_cache_save_pending;
  //! Per-book opt-in for collapsing visually hard-wrapped MOBI prose.
  bool mobi_line_wrap_fix;
  //! Remembers which wrap-fix state produced the currently cached pages.
  bool parsed_with_mobi_line_wrap_fix;

  Book(const BookContext &ctx);
  ~Book();
  format_t format;
  IStatusReporter *GetStatusReporter();
  Text *GetText();
  Prefs *GetPrefs();
  int GetParagraphSpacing();
  int GetParagraphIndent();
  int GetOrientation();
  void DrawBottomGradientBackground();
  inline std::string GetAuthor() { return author; }
  inline bool HasBrowserDisplayNameCache() const {
    return browser_display_name_cached;
  }
  inline const std::string &GetBrowserDisplayNameCache() const {
    return browser_display_name_cache;
  }
  inline void SetBrowserDisplayNameCache(const std::string &name) {
    browser_display_name_cache = name;
    browser_display_name_cached = true;
  }
  inline void ClearBrowserDisplayNameCache() {
    browser_display_name_cache.clear();
    browser_display_name_cached = false;
  }
  inline TocQuality GetTocQuality() const { return toc_quality; }
  inline u16 GetTocDirectCount() const { return toc_direct_count; }
  inline u16 GetTocHeuristicCount() const { return toc_heuristic_count; }
  inline u16 GetTocUnresolvedCount() const { return toc_unresolved_count; }
  inline void SetTocConfidence(TocQuality quality, u16 direct, u16 heuristic,
                               u16 unresolved) {
    toc_quality = quality;
    toc_direct_count = direct;
    toc_heuristic_count = heuristic;
    toc_unresolved_count = unresolved;
  }
  inline void ClearTocConfidence() {
    toc_quality = TOC_QUALITY_UNKNOWN;
    toc_direct_count = 0;
    toc_heuristic_count = 0;
    toc_unresolved_count = 0;
  }
  std::list<u16> *GetBookmarks(void);
  const std::vector<ChapterEntry> &GetChapters() const;
  u16 RegisterInlineImage(const std::string &path);
  void AddChapterAnchor(const std::string &docpath,
                        const std::string &anchor_id);
  bool FindChapterAnchorPage(const std::string &href, u16 *page_out) const;
  size_t GetChapterAnchorCount() const;
  void ClearChapterAnchors();
  void SetChapterDocStartPage(const std::string &docpath, u16 page);
  bool FindChapterDocStartPage(const std::string &href, u16 *page_out) const;
  const std::unordered_map<std::string, u16> &GetChapterDocStartPages() const;
  void ClearChapterDocStartPages();
  const std::string *GetInlineImagePath(u16 id) const;
  u32 GetInlineImageCount() const;
  bool GetInlineImageMetadata(u16 id, InlineImageMetadata *out);
  void SetInlineImageFollowTextLines(u16 id, u8 lines);
  u8 GetInlineImageFollowTextLines(u16 id) const;
  void ClearInlineImages();
  void SetInlineImageProbeZip(void *uf);
  bool StoreFb2InlineImage(const std::string &id,
                           const std::string &base64_data);
  bool PlanInlineImageLayout(Text *ts, u16 image_id, int current_screen,
                             int pen_x, int pen_y, bool line_began,
                             InlineImageContext image_context,
                             InlineImageLayoutPlan *out);
  bool DrawInlineImage(Text *ts, u16 image_id,
                       const InlineImageLayoutPlan *plan = NULL);
  void AddChapter(u16 page, const std::string &title, u8 level = 0);
  void ClearChapters();
  bool IsPdf() const;
  bool IsCbz() const;
  bool IsFixedLayout() const;
  const char *GetFixedLayoutLabel() const;
  bool UsesTextLayoutSettings() const;
  bool SupportsBookmarks() const;
  const char *GetFileName(void);
  const char *GetFolderName(void);
  Page *GetPage();
  Page *GetPage(int i);
  u16 GetPageCount();
  int GetPosition(void);
  const char *GetTitle();
  void SetAuthor(std::string &s);
  void SetFileName(const char *filename);
  void SetFolderName(const char *foldername);
  void SetFolderName(std::string &foldername);
  void SetPage(u16 index);
  void SetPosition(int pos);
  void SetTitle(const char *title);
  Page *AppendPage();
  void ReservePageCapacity(size_t incoming_pages);
  void DrawCurrentView(Text *ts);
  void DrawCurrentMuPdfView(Text *ts);
  void DrawCurrentCbzView(Text *ts);
  void PrepareForOpen();
  u8 OpenPrepared();
  void InitMuPdfView(u16 page_count, fz_context *ctx, fz_document *doc,
                     fz_outline *outline, bool is_new_3ds,
                     app_flow_utils::MuPdfDocumentKind document_kind);
  void InitCbzView(const std::string &archive_path,
                   const std::vector<CbzPageEntry> &entries,
                   bool is_new_3ds);
  void SetFixedLayoutViewportInteraction(bool active);
  void ResetFixedLayoutViewportForNavigation();
  bool ChangeFixedLayoutZoom(int delta);
  bool MoveFixedLayoutViewportToPreview(int touch_x, int touch_y);
  bool JumpFixedLayoutChapter(int delta);
  bool HasPendingFixedLayoutDeferredWork() const;
  u32 GetFixedLayoutDeferredDelayMs() const;
  bool PumpDeferredFixedLayoutWork(u32 budget_ms);
  void CancelFixedLayoutDeferredWork();
  void SetCbzViewportInteraction(bool active);
  void ResetCbzViewport();
  bool ChangeCbzZoom(int delta);
  bool MoveCbzViewportToPreview(int touch_x, int touch_y);
  bool JumpCbzChapter(int delta);
  bool HasPendingCbzDeferredWork() const;
  u32 GetCbzDeferredDelayMs() const;
  bool PumpDeferredCbzWork(u32 budget_ms);
  void CancelCbzDeferredWork();
  void SetMuPdfViewportInteraction(bool active);
  bool ChangeMuPdfZoom(int delta);
  bool MoveMuPdfViewportToPreview(int touch_x, int touch_y);
  bool JumpMuPdfChapter(int delta);
  void PrefetchAdjacentMuPdfPage();
  bool HasPendingMuPdfDeferredWork() const;
  u32 GetMuPdfDeferredDelayMs() const;
  bool PumpDeferredMuPdfWork(u32 budget_ms);
  void CancelMuPdfIncrementalRender();
  void Close();
  u8 Index();
  void IndexHTML();
  u8 Open();
  u8 Parse(bool fulltext = true);
  int ParseHTML();
  bool SupportsAsyncReflowOpen() const;
  bool StartAsyncReflowOpen();
  bool PumpAsyncReflowOpen();
  bool IsAsyncReflowOpenPending() const;
  u8 ConsumeAsyncReflowOpenResult();
  void CancelAsyncReflowOpen();
  bool HasPendingEpubPageCacheSave() const;
  void SetPendingEpubPageCacheSave(bool pending);
  bool HasDeferredMobiParse() const;
  bool ContinueDeferredMobiParse(u32 budget_ms, u16 page_budget = 0);
  void CancelDeferredMobiParse();
  bool IsMobiFile() const;
  bool GetMobiLineWrapFix() const;
  void SetMobiLineWrapFix(bool enabled);
  void MarkMobiRenderSettingsApplied(bool enabled);
  bool NeedsMobiRenderRefresh() const;
  unsigned int GetLayoutRevision() const;
  void SetLayoutRevision(unsigned int revision);
};

#include "formats/cbz/cbz_state.h"
#include "formats/mupdf/mupdf_state.h"

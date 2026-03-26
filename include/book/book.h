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

#include "book/inline_image_layout.h"
#include <3ds.h>
#include <list>
#include <memory>
#include <stddef.h>
#include <string>
#include <unordered_map>
#include <vector>

typedef enum { FORMAT_UNDEF, FORMAT_XHTML, FORMAT_EPUB, FORMAT_PDF } format_t;

class App;
class Page;
class Text;
struct fz_context;
struct fz_outline;
struct pdf_document;

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
  struct PdfState {
    fz_context *ctx;
    pdf_document *doc;
    fz_outline *outline;
    u16 page_count;
    float page_width;
    float page_height;
    bool is_new_3ds;
    bool keep_preview_cache;
    bool keep_tile_cache;
    int max_zoom_index;
    int zoom_index;
    float viewport_center_x;
    float viewport_center_y;
    int cached_preview_page;
    int cached_preview_width;
    int cached_preview_height;
    float cached_preview_content_left;
    float cached_preview_content_top;
    float cached_preview_content_width;
    float cached_preview_content_height;
    std::vector<u16> cached_preview_pixels;
    // Full-page bitmap rendered at the current zoom level.
    // Only invalidated on page or zoom change — NOT on viewport pan —
    // so viewport panning is a cheap crop+blit from this cache.
    int cached_zoom_page;
    int cached_zoom_index;
    int cached_zoom_width;
    int cached_zoom_height;
    std::vector<u16> cached_zoom_pixels;

    PdfState()
        : ctx(NULL), doc(NULL), outline(NULL), page_count(0),
          page_width(612.0f), page_height(792.0f), is_new_3ds(false),
          keep_preview_cache(true), keep_tile_cache(false), max_zoom_index(3),
          zoom_index(2), viewport_center_x(0.5f), viewport_center_y(0.5f),
          cached_preview_page(-1), cached_preview_width(0),
          cached_preview_height(0), cached_preview_content_left(0.0f),
          cached_preview_content_top(0.0f), cached_preview_content_width(1.0f),
          cached_preview_content_height(1.0f), cached_zoom_page(-1),
          cached_zoom_index(-1), cached_zoom_width(0),
          cached_zoom_height(0) {}
  };
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
  size_t inline_image_cache_bytes;
  TocQuality toc_quality;
  u16 toc_direct_count;
  u16 toc_heuristic_count;
  u16 toc_unresolved_count;
  std::vector<Page *> pages;
  PdfState *pdf_state;
  App *app; //! pointer to the App instance.
  unsigned int layout_revision;

  void ClearInlineImageCache();
  bool LoadInlineImageSource(u16 image_id, std::vector<u8> *out,
                             std::string *resolved_path = NULL);
  bool EnsureInlineImageMetadata(u16 image_id, InlineImageMetadata *out);
  void ResetPdfState();

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
  //! Per-book opt-in for collapsing visually hard-wrapped MOBI prose.
  bool mobi_line_wrap_fix;
  //! Remembers which wrap-fix state produced the currently cached pages.
  bool parsed_with_mobi_line_wrap_fix;

  Book(App *app);
  ~Book();
  format_t format;
  inline App *GetApp() { return app; }
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
  void DrawCurrentView(Text *ts);
  void InitPdfView(u16 page_count, fz_context *ctx, pdf_document *doc,
                   fz_outline *outline, bool is_new_3ds);
  bool ChangePdfZoom(int delta);
  bool MovePdfViewportToPreview(int touch_x, int touch_y);
  bool JumpPdfChapter(int delta);
  void Close();
  u8 Index();
  void IndexHTML();
  u8 Open();
  u8 Parse(bool fulltext = true);
  int ParseHTML();
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

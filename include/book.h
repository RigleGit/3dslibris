/*
    3dslibris - book.h
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Book domain model (metadata, pages, chapters, bookmarks, parse context).
    - Tracks source path/format and reading progress for persistence.
    - Adds 3DS-side helpers for browser labels and inline image cache bookkeeping.
*/

#pragma once

#include "app.h"
#include "page.h"
#include <3ds.h>
#include <list>
#include <stddef.h>
#include <unordered_map>
#include <string>
#include <vector>

typedef enum { FORMAT_UNDEF, FORMAT_XHTML, FORMAT_EPUB } format_t;

namespace xml::book {
void start(void *data, const char *el, const char **attr);
void chardata(void *data, const char *txt, int txtlen);
void end(void *data, const char *el);
void instruction(void *data, const char *target, const char *pidata);
int unknown(void *encodingHandlerData, const XML_Char *name,
            XML_Encoding *info);
void fallback(void *data, const XML_Char *s, int len);
} // namespace xml::book

namespace xml::book::metadata {
void start(void *userdata, const char *el, const char **attr);
void chardata(void *userdata, const char *txt, int txtlen);
void end(void *userdata, const char *el);
} // namespace xml::book::metadata

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
  struct InlineImageCacheEntry {
    u16 image_id;
    u16 screen_h;
    u16 bg565;
    u16 start_x;
    u16 start_y;
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
  std::vector<std::string> inline_images;
  std::unordered_map<std::string, u16> chapter_anchor_pages;
  std::unordered_map<std::string, u16> chapter_doc_start_pages;
  std::unordered_map<std::string, std::vector<u8>> fb2_inline_images;
  size_t fb2_inline_images_bytes;
  std::list<InlineImageCacheEntry> inline_image_cache;
  size_t inline_image_cache_bytes;
  TocQuality toc_quality;
  u16 toc_direct_count;
  u16 toc_heuristic_count;
  u16 toc_unresolved_count;
  std::vector<class Page *> pages;
  App *app; //! pointer to the App instance.

  void ClearInlineImageCache();
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
  void AddChapterAnchor(const std::string &docpath, const std::string &anchor_id);
  bool FindChapterAnchorPage(const std::string &href, u16 *page_out) const;
  size_t GetChapterAnchorCount() const;
  void ClearChapterAnchors();
  void SetChapterDocStartPage(const std::string &docpath, u16 page);
  bool FindChapterDocStartPage(const std::string &href, u16 *page_out) const;
  const std::unordered_map<std::string, u16> &GetChapterDocStartPages() const;
  void ClearChapterDocStartPages();
  const std::string *GetInlineImagePath(u16 id) const;
  void ClearInlineImages();
  bool StoreFb2InlineImage(const std::string &id,
                           const std::string &base64_data);
  bool DrawInlineImage(Text *ts, u16 image_id);
  void AddChapter(u16 page, const std::string &title, u8 level = 0);
  void ClearChapters();
  int GetNextBookmark(void);
  int GetPreviousBookmark(void);
  int GetNextBookmarkedPage(void);
  int GetPreviousBookmarkedPage(void);
  const char *GetFileName(void);
  const char *GetFolderName(void);
  Page *GetPage();
  Page *GetPage(int i);
  u16 GetPageCount();
  int GetPosition(void);
  int GetPosition(int offset);
  const char *GetTitle();
  void SetAuthor(std::string &s);
  void SetFileName(const char *filename);
  void SetFolderName(const char *foldername);
  void SetFolderName(std::string &foldername);
  void SetPage(u16 index);
  void SetPosition(int pos);
  void SetTitle(const char *title);
  Page *AppendPage();
  Page *AdvancePage();
  Page *RetreatPage();
  void Close();
  u8 Index();
  void IndexHTML();
  u8 Open();
  u8 Parse(bool fulltext = true);
  int ParseHTML();
  bool HasDeferredMobiParse() const;
  bool ContinueDeferredMobiParse(u32 budget_ms, u16 page_budget = 0);
  void CancelDeferredMobiParse();
};

/*
    3dslibris - book.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Core book state/container logic shared across EPUB/FB2/TXT/RTF/ODT.
    - Chapter/bookmark management and TOC target resolution helpers.
    - Page ownership/lifetime and parser integration points.
*/

#include "book/book.h"

#include "book/book_context.h"
#include "shared/debug_log.h"
#include "book/heading_layout.h"
#include "formats/common/href_normalization.h"
#include "formats/epub/epub.h"
#include "formats/epub/epub_page_cache.h"
#include "formats/mobi/mobi_page_cache.h"
#include "shared/main.h"
#include "book/page.h"
#include "book/page_buffer_utils.h"
#include "parse.h"
#include "book/reflow_cache_save_utils.h"
#include "shared/home_button_guard.h"
#include "ui/screen_constants.h"
#include "shared/text_layout_utils.h"
#include "shared/text_unicode_utils.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

namespace {
static bool HasExtCaseInsensitive(const std::string &name, const char *ext) {
  if (!ext)
    return false;
  const size_t name_len = name.size();
  const size_t ext_len = strlen(ext);
  if (name_len < ext_len)
    return false;
  const size_t start = name_len - ext_len;
  for (size_t i = 0; i < ext_len; i++) {
    unsigned char a = (unsigned char)name[start + i];
    unsigned char b = (unsigned char)ext[i];
    if (a >= 'A' && a <= 'Z')
      a = (unsigned char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z')
      b = (unsigned char)(b - 'A' + 'a');
    if (a != b)
      return false;
  }
  return true;
}

static const u32 kEtaSampleMinMs = 600;
static const u32 kEtaSampleMaxMs = 240000;
static const float kEtaEmaAlpha = 0.20f;

static int ClampPositionToPageCount(int pos, u16 page_count) {
  if (page_count == 0)
    return 0;
  if (pos < 0)
    return 0;
  if (pos >= (int)page_count)
    return (int)page_count - 1;
  return pos;
}

static int RemainingMinutesFromMsPerPage(int remaining_pages,
                                         float ms_per_page) {
  if (remaining_pages <= 0)
    return 0;
  if (ms_per_page <= 0.0f)
    return -1;
  const float total_ms = (float)remaining_pages * ms_per_page;
  int minutes = (int)((total_ms + 59999.0f) / 60000.0f);
  if (minutes < 1)
    minutes = 1;
  return minutes;
}

} // namespace

Book::Book(const BookContext &c) : ctx(c) {
  // State / Formats
  mupdf_state = NULL;
  cbz_state = NULL;
  reflow_worker_state = NULL;
  inline_image_probe_uf = NULL;
  format = FORMAT_UNDEF;

  // Position state / basic rendering
  position = 0;
  eta_ms_per_page_ = 0.0f;
  eta_last_adjacent_turn_ms_ = 0;
  eta_samples_ = 0;
  last_opened_time = 0;
  coverPixels = nullptr;
  coverWidth = 0;
  coverHeight = 0;

  // Cover state / metadata parsing / TOC resolution
  coverAttempts = 0;
  coverRetryAfterMs = 0;
  metadataIndexTried = false;
  metadataIndexed = false;
  tocResolveTried = false;
  tocResolved = false;

  // Cache state / rendering heuristics
  epub_page_cache_save_pending = false;
  mobi_page_cache_save_pending = false;
  mobi_line_wrap_fix = false;
  parsed_with_mobi_line_wrap_fix = false;
  style_font_size_override = -1;
  style_line_spacing_override = -1;
  style_paragraph_spacing_override = -1;
  style_publisher_text_indent_override = -1;
  style_publisher_block_margins_override = -1;
  browser_display_name_cached = false;
  browser_folder_entry = false;
  inline_image_zip_index_built = false;
  mobi_inline_index_ready = false;
  mobi_first_image_index = 0;
  fb2_inline_images_bytes = 0;
  inline_image_cache_bytes = 0;
  layout_revision = 0;

  // Session state
  open_session_id_ = 0;
  open_abort_requested_ = false;
  focused_inline_link_index = -1;
  ClearTocConfidence();
}

// Destructor for the Book class, responsible for cleaning up resources and
// closing the book if it's still open. Also logs the book closure if a status
// reporter is available.
Book::~Book() {
  Close();
  if (coverPixels) {
    delete[] coverPixels;
    coverPixels = nullptr;
  }
}

IStatusReporter *Book::GetStatusReporter() {
  return ctx.status_reporter;
}

Text *Book::GetText() {
  return ctx.text;
}

Prefs *Book::GetPrefs() {
  return ctx.prefs;
}

int Book::GetParagraphSpacing() {
  if (style_paragraph_spacing_override >= 0)
    return style_paragraph_spacing_override;
  return ctx.paragraph_spacing ? *ctx.paragraph_spacing : 0;
}

int Book::GetParagraphIndent() {
  return ctx.paragraph_indent ? *ctx.paragraph_indent : 0;
}

int Book::GetStyleFontSizeOverride() const {
  return style_font_size_override;
}

void Book::SetStyleFontSizeOverride(int value) {
  style_font_size_override = value;
}

int Book::GetStyleLineSpacingOverride() const {
  return style_line_spacing_override;
}

void Book::SetStyleLineSpacingOverride(int value) {
  style_line_spacing_override = value;
}

bool Book::GetPublisherTextIndentEnabled() const {
  if (style_publisher_text_indent_override >= 0)
    return style_publisher_text_indent_override != 0;
  return ctx.publisher_text_indent ? *ctx.publisher_text_indent : true;
}

bool Book::GetPublisherBlockMarginsEnabled() const {
  if (style_publisher_block_margins_override >= 0)
    return style_publisher_block_margins_override != 0;
  return ctx.publisher_block_margins ? *ctx.publisher_block_margins : true;
}

int Book::GetStyleParagraphSpacingOverride() const {
  return style_paragraph_spacing_override;
}

void Book::SetStyleParagraphSpacingOverride(int value) {
  style_paragraph_spacing_override = value;
}

int Book::GetStylePublisherTextIndentOverride() const {
  return style_publisher_text_indent_override;
}

void Book::SetStylePublisherTextIndentOverride(int value) {
  style_publisher_text_indent_override = value;
}

int Book::GetStylePublisherBlockMarginsOverride() const {
  return style_publisher_block_margins_override;
}

void Book::SetStylePublisherBlockMarginsOverride(int value) {
  style_publisher_block_margins_override = value;
}

int Book::GetOrientation() {
  return ctx.orientation ? *ctx.orientation : 0;
}

void Book::DrawBottomGradientBackground() {
  if (ctx.draw_background)
    ctx.draw_background(ctx.draw_background_user_data);
}

void Book::DrawTopGradientBackground() {
  if (ctx.draw_top_background)
    ctx.draw_top_background(ctx.draw_top_background_user_data);
}

void Book::NotifySpineProgress(unsigned done, unsigned total) {
  if (ctx.on_spine_progress)
    ctx.on_spine_progress(done, total, ctx.on_spine_progress_user_data);
}

void Book::SetFolderName(const char *name) {
  foldername = name;
}

void Book::SetFileName(const char *name) {
  filename = name;
  ClearBrowserDisplayNameCache();
}

void Book::SetTitle(const char *name) {
  title = name;
  ClearBrowserDisplayNameCache();
}

void Book::SetAuthor(const std::string &name) {
  author = name;
}

void Book::SetFolderName(const std::string &name) {
  foldername = name;
}

void Book::SetBrowserFolderEntry(const std::string &path,
                                 const std::string &display_name,
                                 const std::string &cover_path)
{
  browser_folder_entry = true;
  browser_folder_path = path;
  browser_folder_display_name = display_name;
  browser_folder_cover_path = cover_path;
  foldername = path;
  filename.clear();
  title = display_name;
  author.clear();
  format = FORMAT_UNDEF;
  ClearBrowserDisplayNameCache();
}

std::list<u16> &Book::GetBookmarks() {
  return bookmarks;
}

const std::list<u16> &Book::GetBookmarks() const {
  return bookmarks;
}
const std::vector<ChapterEntry> &Book::GetChapters() const {
  return chapters;
}
u16 Book::RegisterInlineLinkHref(const std::string &href) {
  if (href.empty())
    return 0;
  std::unordered_map<std::string, u16>::const_iterator hit =
      inline_link_href_index.find(href);
  if (hit != inline_link_href_index.end())
    return hit->second;
  if (inline_link_hrefs.size() >= 0xFFFFu)
    return 0;
  const u16 id = (u16)(inline_link_hrefs.size() + 1u);
  inline_link_hrefs.push_back(href);
  inline_link_href_index[href] = id;
  return id;
}

const std::string *Book::GetInlineLinkHref(u16 id) const {
  if (id == 0)
    return NULL;
  const size_t index = (size_t)(id - 1u);
  if (index >= inline_link_hrefs.size())
    return NULL;
  return &inline_link_hrefs[index];
}

u32 Book::GetInlineLinkHrefCount() const {
  return (u32)inline_link_hrefs.size();
}

void Book::ClearInlineLinks() {
  inline_link_hrefs.clear();
  inline_link_href_index.clear();
  focused_inline_link_index = -1;
}

void Book::AddChapterAnchor(const std::string &docpath,
                            const std::string &anchor_id) {
  if (docpath.empty() || anchor_id.empty())
    return;
  if (chapter_anchor_pages.size() >= 8192)
    return;

  std::string key = href_normalization::BuildAnchorKey(docpath, anchor_id);
  if (key.empty())
    return;

  if (chapter_anchor_pages.find(key) == chapter_anchor_pages.end()) {
    chapter_anchor_pages[key] = GetPageCount();
  }
}

void Book::SetChapterAnchorPage(const std::string &href, u16 page) {
  std::string key = href_normalization::NormalizeAnchorHrefKey(href);
  if (key.empty())
    return;
  if (chapter_anchor_pages.find(key) == chapter_anchor_pages.end())
    chapter_anchor_pages[key] = page;
}

/*
 * FindChapterAnchorPage — resolve a TOC href to the page that contains
 * the corresponding anchor.
 *
 * Real-world EPUBs (especially hand-converted or poorly-tooled ones) can have
 * inconsistent paths or casing between the TOC and the actual XHTML documents.
 * This function uses a multi-tier lookup to maximise the chance of finding the
 * correct page:
 *
 *  1. Exact match on the normalised href key (path#anchor).
 *  2. Case-insensitive exact match (covers case-folding mismatches).
 *  3. Same-document match: find an anchor in the same document as the target
 *     href (determined by doc-start page), even if paths differ textually.
 *  4. Fuzzy anchor match: if the anchor ID's "token" prefix or trailing digits
 *     match another anchor in the same document, accept it.
 *  5. Basename match: same anchor ID under the same filename, ignoring dirs.
 *  6. Global anchor match: any registered anchor with the same ID, regardless
 *     of which document it appears in.
 *
 * Ambiguous results (multiple distinct pages for the same tier) are skipped in
 * favour of the next tier to avoid silently jumping to the wrong page.
 */
bool Book::FindChapterAnchorPage(const std::string &href, u16 *page_out) const {
  if (!page_out)
    return false;
  std::string key = href_normalization::NormalizeAnchorHrefKey(href);
  if (key.empty())
    return false;

  auto hit = chapter_anchor_pages.find(key);
  if (hit != chapter_anchor_pages.end()) {
    *page_out = hit->second;
    return true;
  }

  // Fallback only for malformed files with inconsistent anchor case.
  std::string key_lc = href_normalization::ToLowerAsciiLocal(key);
  for (const auto &kv : chapter_anchor_pages) {
    if (href_normalization::ToLowerAsciiLocal(kv.first) == key_lc) {
      *page_out = kv.second;
      return true;
    }
  }

  // Robust fallback for malformed EPUBs where TOC path and parsed doc path
  // differ but anchor IDs are still consistent.
  size_t hash = key.find('#');
  if (hash != std::string::npos && hash + 1 < key.size()) {
    std::string key_path_lc =
        href_normalization::ToLowerAsciiLocal(key.substr(0, hash));
    std::string key_base_lc = href_normalization::ToLowerAsciiLocal(
        href_normalization::BasenamePathLocal(key_path_lc));
    std::string key_anchor_lc =
        href_normalization::ToLowerAsciiLocal(key.substr(hash + 1));
    u16 target_doc_page = 0;
    bool has_target_doc =
        FindChapterDocStartPage(key_path_lc, &target_doc_page);

    bool path_anchor_found = false;
    u16 path_anchor_page = 0;
    bool path_anchor_ambiguous = false;
    bool base_anchor_found = false;
    u16 base_anchor_page = 0;
    bool base_anchor_ambiguous = false;
    bool anchor_found = false;
    u16 anchor_page = 0;
    bool anchor_ambiguous = false;
    bool fuzzy_doc_found = false;
    u16 fuzzy_doc_page = 0;
    bool fuzzy_doc_ambiguous = false;
    const std::string key_token =
        href_normalization::AnchorTokenKey(key_anchor_lc);
    const std::string key_digits =
        href_normalization::AnchorDigits(key_anchor_lc);

    for (const auto &kv : chapter_anchor_pages) {
      size_t kv_hash = kv.first.find('#');
      if (kv_hash == std::string::npos || kv_hash + 1 >= kv.first.size())
        continue;
      std::string kv_path_lc =
          href_normalization::ToLowerAsciiLocal(kv.first.substr(0, kv_hash));
      std::string kv_base_lc = href_normalization::ToLowerAsciiLocal(
          href_normalization::BasenamePathLocal(kv_path_lc));
      std::string kv_anchor_lc =
          href_normalization::ToLowerAsciiLocal(kv.first.substr(kv_hash + 1));
      bool exact_anchor = (kv_anchor_lc == key_anchor_lc);

      if (exact_anchor) {
        if (!anchor_found) {
          anchor_found = true;
          anchor_page = kv.second;
        } else if (anchor_page != kv.second) {
          anchor_ambiguous = true;
        }

        if (has_target_doc) {
          u16 candidate_doc_page = 0;
          if (FindChapterDocStartPage(kv.first, &candidate_doc_page) &&
              candidate_doc_page == target_doc_page) {
            if (!path_anchor_found) {
              path_anchor_found = true;
              path_anchor_page = kv.second;
            } else if (path_anchor_page != kv.second) {
              path_anchor_ambiguous = true;
            }
          }
        }

        if (!key_base_lc.empty() && kv_base_lc == key_base_lc) {
          if (!base_anchor_found) {
            base_anchor_found = true;
            base_anchor_page = kv.second;
          } else if (base_anchor_page != kv.second) {
            base_anchor_ambiguous = true;
          }
        }
      }

      if (has_target_doc && !path_anchor_found && !key_token.empty()) {
        u16 candidate_doc_page = 0;
        if (FindChapterDocStartPage(kv.first, &candidate_doc_page) &&
            candidate_doc_page == target_doc_page) {
          std::string cand_token =
              href_normalization::AnchorTokenKey(kv_anchor_lc);
          std::string cand_digits =
              href_normalization::AnchorDigits(kv_anchor_lc);
          bool fuzzy_match = false;
          if (!cand_token.empty() && cand_token == key_token) {
            fuzzy_match = true;
          } else if (!key_digits.empty() && !cand_digits.empty() &&
                     cand_digits == key_digits) {
            fuzzy_match = true;
          } else if (!key_digits.empty() && !cand_token.empty() &&
                     cand_token.size() > key_digits.size() &&
                     cand_token.size() <= key_digits.size() + 4 &&
                     cand_token.compare(cand_token.size() - key_digits.size(),
                                        key_digits.size(), key_digits) == 0) {
            fuzzy_match = true;
          }

          if (fuzzy_match) {
            if (!fuzzy_doc_found) {
              fuzzy_doc_found = true;
              fuzzy_doc_page = kv.second;
            } else if (fuzzy_doc_page != kv.second) {
              fuzzy_doc_ambiguous = true;
            }
          }
        }
      }
    }

    if (path_anchor_found && !path_anchor_ambiguous) {
      *page_out = path_anchor_page;
      return true;
    }
    if (fuzzy_doc_found && !fuzzy_doc_ambiguous) {
      *page_out = fuzzy_doc_page;
      return true;
    }
    if (base_anchor_found && !base_anchor_ambiguous) {
      *page_out = base_anchor_page;
      return true;
    }
    if (anchor_found && !anchor_ambiguous) {
      *page_out = anchor_page;
      return true;
    }
  }

  return false;
}

size_t Book::GetChapterAnchorCount() const {
  return chapter_anchor_pages.size();
}

const std::unordered_map<std::string, u16> &
Book::GetChapterAnchorPages() const {
  return chapter_anchor_pages;
}

void Book::ClearChapterAnchors() {
  chapter_anchor_pages.clear();
}

void Book::SetChapterDocStartPage(const std::string &docpath, u16 page) {
  if (docpath.empty())
    return;
  std::string key =
      href_normalization::NormalizeDocStartPathKey(docpath, false);
  if (key.empty())
    return;
  if (chapter_doc_start_pages.find(key) == chapter_doc_start_pages.end())
    chapter_doc_start_pages[key] = page;
}

bool Book::FindChapterDocStartPage(const std::string &href,
                                   u16 *page_out) const {
  if (!page_out)
    return false;
  std::string key = href_normalization::NormalizeDocStartPathKey(href, true);
  if (key.empty())
    return false;

  auto hit = chapter_doc_start_pages.find(key);
  if (hit != chapter_doc_start_pages.end()) {
    *page_out = hit->second;
    return true;
  }

  std::string key_lc = href_normalization::ToLowerAsciiLocal(key);
  for (const auto &kv : chapter_doc_start_pages) {
    if (href_normalization::ToLowerAsciiLocal(kv.first) == key_lc) {
      *page_out = kv.second;
      return true;
    }
  }

  return false;
}

const std::unordered_map<std::string, u16> &
Book::GetChapterDocStartPages() const {
  return chapter_doc_start_pages;
}

void Book::ClearChapterDocStartPages() {
  chapter_doc_start_pages.clear();
}

void Book::AddChapter(u16 page, const std::string &title, u8 level) {
  ChapterEntry entry;
  entry.page = page;
  entry.level = level;
  entry.title = title;
  chapters.push_back(entry);
}

void Book::ClearChapters() {
  chapters.clear();
}

bool Book::IsPdf() const {
  return format == FORMAT_PDF;
}

bool Book::IsCbz() const {
  return format == FORMAT_CBZ;
}

bool Book::IsFixedLayout() const {
  return IsPdf() || IsCbz();
}

const char *Book::GetFixedLayoutLabel() const {
  if (IsCbz())
    return "CBZ";
  if (IsPdf() && mupdf_state) {
    return app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind);
  }
  if (IsPdf())
    return "PDF";
  return "BOOK";
}

bool Book::UsesTextLayoutSettings() const {
  return !IsFixedLayout();
}

int Book::GetFocusedInlineLinkIndex() const {
  return focused_inline_link_index;
}

void Book::SetFocusedInlineLinkIndex(int index) {
  focused_inline_link_index = index;
}

void Book::ClearFocusedInlineLink() {
  focused_inline_link_index = -1;
}

bool Book::SupportsBookmarks() const {
  return !IsFixedLayout();
}

Page *Book::GetPage() {
  return pages[position];
}

Page *Book::GetPage(int index) {
  return pages[index];
}

u16 Book::GetPageCount() {
  if (IsPdf() && mupdf_state)
    return mupdf_state->page_count;
  if (IsCbz() && cbz_state)
    return cbz_state->page_count;
  return pages.size();
}

const char *Book::GetTitle() {
  return title.c_str();
}

const char *Book::GetFileName() {
  return filename.c_str();
}

const char *Book::GetFolderName() {
  return foldername.c_str();
}

int Book::GetPosition() {
  return position;
}

void Book::SetPage(u16 index) {
  SetPosition((int)index);
}

void Book::SetPosition(int pos) {
  const int prev = position;
  const int next = ClampPositionToPageCount(pos, GetPageCount());
  position = next;

  const int delta = next - prev;
  const int abs_delta = delta >= 0 ? delta : -delta;
  if (abs_delta > 1) {
    eta_last_adjacent_turn_ms_ = 0;
    return;
  }
  if (abs_delta != 1)
    return;

  const u32 now_ms = (u32)osGetTime();
  if (eta_last_adjacent_turn_ms_ != 0 && now_ms > eta_last_adjacent_turn_ms_) {
    const u32 elapsed_ms = now_ms - eta_last_adjacent_turn_ms_;
    if (elapsed_ms >= kEtaSampleMinMs && elapsed_ms <= kEtaSampleMaxMs) {
      const float sample = (float)elapsed_ms;
      if (eta_samples_ == 0)
        eta_ms_per_page_ = sample;
      else
        eta_ms_per_page_ = eta_ms_per_page_ * (1.0f - kEtaEmaAlpha) +
                           sample * kEtaEmaAlpha;
      if (eta_samples_ < 0xFFFF)
        eta_samples_++;
    }
  }
  eta_last_adjacent_turn_ms_ = now_ms;
}

void Book::ResetReadingPaceEstimate() {
  eta_ms_per_page_ = 0.0f;
  eta_last_adjacent_turn_ms_ = 0;
  eta_samples_ = 0;
}

bool Book::HasReadingPaceEstimate() const {
  return eta_samples_ >= 3 && eta_ms_per_page_ > 0.0f;
}

int Book::EstimateRemainingBookMinutes() const {
  const int page_count =
      (IsPdf() && mupdf_state) ? (int)mupdf_state->page_count
                               : ((IsCbz() && cbz_state) ? (int)cbz_state->page_count
                                                         : (int)pages.size());
  if (page_count <= 0)
    return -1;
  if (!HasReadingPaceEstimate())
    return -1;
  int pos = position;
  if (pos < 0)
    pos = 0;
  if (pos >= page_count)
    pos = page_count - 1;
  return RemainingMinutesFromMsPerPage(page_count - 1 - pos, eta_ms_per_page_);
}

int Book::EstimateRemainingChapterMinutes() const {
  const int page_count =
      (IsPdf() && mupdf_state) ? (int)mupdf_state->page_count
                               : ((IsCbz() && cbz_state) ? (int)cbz_state->page_count
                                                         : (int)pages.size());
  if (page_count <= 0)
    return -1;
  if (!HasReadingPaceEstimate())
    return -1;
  int pos = position;
  if (pos < 0)
    pos = 0;
  if (pos >= page_count)
    pos = page_count - 1;

  if (chapters.empty())
    return EstimateRemainingBookMinutes();

  int chapter_end = page_count - 1;
  for (size_t i = 0; i < chapters.size(); i++) {
    if ((int)chapters[i].page > pos) {
      chapter_end = (int)chapters[i].page - 1;
      break;
    }
  }
  if (chapter_end < pos)
    chapter_end = pos;

  return RemainingMinutesFromMsPerPage(chapter_end - pos, eta_ms_per_page_);
}

Page *Book::AppendPage() {
  Page *page = new Page(this);
  pages.push_back(page);
  return page;
}

void Book::ReservePageCapacity(size_t incoming_pages) {
  const size_t required_capacity =
      page_buffer_utils::RequiredPageVectorCapacity(
          pages.size(), pages.capacity(), incoming_pages);
  if (required_capacity > pages.capacity())
    pages.reserve(required_capacity);
}

void Book::FlushPendingCacheSaves() {
  IStatusReporter *r = GetStatusReporter();
  const bool flush_epub =
      reflow_cache_save_utils::ShouldFlushDeferredCacheSaveOnClose(
          epub_page_cache_save_pending, IsAsyncReflowOpenPending(),
          (unsigned int)GetPageCount());
  const bool flush_mobi =
      reflow_cache_save_utils::ShouldFlushDeferredCacheSaveOnClose(
          mobi_page_cache_save_pending, IsAsyncReflowOpenPending(),
          (unsigned int)GetPageCount());
  if (flush_epub) {
    DBG_LOGF(r, "BOOK flush-cache: save-epub begin pages=%d book=%s",
             (int)pages.size(), filename.c_str());
    HomeButtonGuard home_guard;
    epub_page_cache::SavePending(this, true);
    DBG_LOGF(r, "BOOK flush-cache: save-epub done book=%s", filename.c_str());
  }
  if (flush_mobi) {
    DBG_LOGF(r, "BOOK flush-cache: save-mobi begin pages=%d book=%s",
             (int)pages.size(), filename.c_str());
    HomeButtonGuard home_guard;
    mobi_page_cache::SavePending(this);
    DBG_LOGF(r, "BOOK flush-cache: save-mobi done book=%s", filename.c_str());
  }
}

void Book::Close() {
  const bool flush_pending_epub_cache =
      reflow_cache_save_utils::ShouldFlushDeferredCacheSaveOnClose(
          epub_page_cache_save_pending, IsAsyncReflowOpenPending(),
          (unsigned int)GetPageCount());
  const bool flush_pending_mobi_cache =
      reflow_cache_save_utils::ShouldFlushDeferredCacheSaveOnClose(
          mobi_page_cache_save_pending, IsAsyncReflowOpenPending(),
          (unsigned int)GetPageCount());
  CancelAsyncReflowOpen();
  if (flush_pending_epub_cache) {
    HomeButtonGuard home_guard;
    epub_page_cache::SavePending(this, true);
  }
  if (flush_pending_mobi_cache) {
    HomeButtonGuard home_guard;
    mobi_page_cache::SavePending(this);
  }
  epub_page_cache_save_pending = false;
  mobi_page_cache_save_pending = false;
  std::vector<Page *>::iterator it = pages.begin();
  while (it != pages.end()) {
    delete *it;
    *it = nullptr;
    ++it;
  }
  {
    std::vector<Page *>().swap(pages);
  }
  ResetReflowWorkerState();
  ResetCbzState();
  ResetMuPdfState();
  {
    std::vector<ChapterEntry>().swap(chapters);
  }
  ClearChapterAnchors();
  ClearChapterDocStartPages();
  ClearInlineLinks();
  ClearInlineImages();
  ClearTocConfidence();
  ResetReadingPaceEstimate();
  open_session_id_ = 0;
  open_abort_requested_ = false;
}

void Book::ResetCbzFailureState() {
  if (!IsCbz() || !cbz_state)
    return;
  cbz_state->failed_page = -1;
  cbz_state->logged_failed_page = -1;
  cbz_state->last_error.clear();
}

bool Book::IsMobiFile() const {
  return HasExtCaseInsensitive(filename, ".mobi");
}

bool Book::GetMobiLineWrapFix() const {
  return mobi_line_wrap_fix;
}

void Book::SetMobiLineWrapFix(bool enabled) {
  mobi_line_wrap_fix = enabled;
}

void Book::MarkMobiRenderSettingsApplied(bool enabled) {
  parsed_with_mobi_line_wrap_fix = enabled;
}

bool Book::NeedsMobiRenderRefresh() const {
  return IsMobiFile() && parsed_with_mobi_line_wrap_fix != mobi_line_wrap_fix;
}

bool Book::HasPendingEpubPageCacheSave() const {
  return epub_page_cache_save_pending;
}

void Book::SetPendingEpubPageCacheSave(bool pending) {
  epub_page_cache_save_pending = pending;
}

void Book::SetPendingEpubPageCacheSaveWithParams(
    int pixel_size, int line_spacing, int paragraph_spacing,
    int paragraph_indent, int orientation, int margin_left, int margin_right,
    int margin_top, int margin_bottom, const char *regular_font) {
  epub_page_cache_save_pending = true;
  epub_cache_save_params.pixel_size = pixel_size;
  epub_cache_save_params.line_spacing = line_spacing;
  epub_cache_save_params.paragraph_spacing = paragraph_spacing;
  epub_cache_save_params.paragraph_indent = paragraph_indent;
  epub_cache_save_params.orientation = orientation;
  epub_cache_save_params.margin_left = margin_left;
  epub_cache_save_params.margin_right = margin_right;
  epub_cache_save_params.margin_top = margin_top;
  epub_cache_save_params.margin_bottom = margin_bottom;
  epub_cache_save_params.regular_font = regular_font ? regular_font : "";
}

const Book::EpubCacheSaveParams &Book::GetEpubCacheSaveParams() const {
  return epub_cache_save_params;
}

bool Book::HasPendingMobiPageCacheSave() const {
  return mobi_page_cache_save_pending;
}

void Book::SetPendingMobiPageCacheSave(bool pending) {
  mobi_page_cache_save_pending = pending;
}

void Book::SetPendingMobiPageCacheSaveWithParams(
    int pixel_size, int line_spacing, int paragraph_spacing,
    int paragraph_indent, int orientation, int margin_left, int margin_right,
    int margin_top, int margin_bottom, const char *regular_font,
    bool line_wrap_fix_enabled) {
  mobi_page_cache_save_pending = true;
  mobi_cache_save_params.pixel_size = pixel_size;
  mobi_cache_save_params.line_spacing = line_spacing;
  mobi_cache_save_params.paragraph_spacing = paragraph_spacing;
  mobi_cache_save_params.paragraph_indent = paragraph_indent;
  mobi_cache_save_params.orientation = orientation;
  mobi_cache_save_params.margin_left = margin_left;
  mobi_cache_save_params.margin_right = margin_right;
  mobi_cache_save_params.margin_top = margin_top;
  mobi_cache_save_params.margin_bottom = margin_bottom;
  mobi_cache_save_params.regular_font = regular_font ? regular_font : "";
  mobi_cache_save_params.line_wrap_fix_enabled = line_wrap_fix_enabled;
}

const Book::MobiCacheSaveParams &Book::GetMobiCacheSaveParams() const {
  return mobi_cache_save_params;
}

unsigned int Book::GetLayoutRevision() const {
  return layout_revision;
}

void Book::SetLayoutRevision(unsigned int revision) {
  layout_revision = revision;
}

unsigned int Book::GetOpenSessionId() const {
  return open_session_id_;
}

void Book::SetOpenSessionId(unsigned int session_id) {
  open_session_id_ = session_id;
}

bool Book::IsOpenAbortRequested() const {
  return open_abort_requested_;
}

void Book::RequestAbortOpen() {
  open_abort_requested_ = true;
}

void Book::ClearOpenAbortRequest() {
  open_abort_requested_ = false;
}

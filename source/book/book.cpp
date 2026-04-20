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
#include "debug_log.h"
#include "book/heading_layout.h"
#include "formats/epub/epub.h"
#include "formats/epub/epub_page_cache.h"
#include "main.h"
#include "book/page.h"
#include "book/page_buffer_utils.h"
#include "parse.h"
#include "book/reflow_cache_save_utils.h"
#include "screen_constants.h"
#include "shared/text_layout_utils.h"
#include "shared/text_unicode_utils.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

namespace
{
  // Helper function to check if the file extension of a given name matches the specified extension, case-insensitively.
  static bool HasExtCaseInsensitive(const std::string &name, const char *ext)
  {
    if (!ext)
      return false;
    const size_t name_len = name.size();
    const size_t ext_len = strlen(ext);
    if (name_len < ext_len)
      return false;
    const size_t start = name_len - ext_len;
    for (size_t i = 0; i < ext_len; i++)
    {
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

  // Returns the basename after the last '/' in the path, or the full path if there are no slashes.
  static std::string BasenamePathLocal(const std::string &path)
  {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
      return path;
    if (slash + 1 >= path.size())
      return "";
    return path.substr(slash + 1);
  }

  // Returns a lowercase ASCII version of the basename of the path, used for case-insensitive comparisons of file names.
  static std::string AnchorTokenKey(const std::string &s)
  {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
      unsigned char c = (unsigned char)s[i];
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        out.push_back((char)c);
    }
    return out;
  }

  // Returns only the digit characters from the basename of the path, used for numeric anchor matching.
  static std::string AnchorDigits(const std::string &s)
  {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
      unsigned char c = (unsigned char)s[i];
      if (c >= '0' && c <= '9')
        out.push_back((char)c);
    }
    return out;
  }

  // Helper function to trim leading and trailing paths
  static std::string NormalizePathForAnchor(const std::string &path)
  {
    std::string in = path;
    std::replace(in.begin(), in.end(), '\\', '/');
    while (!in.empty() && in[0] == '/')
      in.erase(in.begin());

    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= in.size(); i++)
    {
      if (i == in.size() || in[i] == '/')
      {
        if (cur == "..")
        {
          if (!parts.empty())
            parts.pop_back();
        }
        else if (!cur.empty() && cur != ".")
        {
          parts.push_back(cur);
        }
        cur.clear();
      }
      else
      {
        cur.push_back(in[i]);
      }
    }

    std::string out;
    for (size_t i = 0; i < parts.size(); i++)
    {
      if (i)
        out.push_back('/');
      out += parts[i];
    }
    return out;
  }

  // Decodes %xx URL-encoded sequences in the input string, returning the decoded result.
  static std::string UrlDecodeComponent(const std::string &input)
  {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); i++)
    {
      if (input[i] == '%' && i + 2 < input.size())
      {
        int value = 0;
        if (sscanf(input.substr(i + 1, 2).c_str(), "%x", &value) == 1)
        {
          out.push_back((char)value);
          i += 2;
          continue;
        }
      }
      out.push_back(input[i]);
    }
    return out;
  }

  // Lowercase ASCII
  static std::string ToLowerAsciiLocal(const std::string &s)
  {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c)
                   { return (char)tolower(c); });
    return out;
  }

  // Produces a key for matching chapter hrefs to anchors by normalizing the path and anchor components of the href.
  static std::string BuildAnchorKey(const std::string &docpath,
                                    const std::string &anchor_raw)
  {
    if (docpath.empty() || anchor_raw.empty())
      return "";

    std::string doc = UrlDecodeComponent(docpath);
    std::string anchor = UrlDecodeComponent(anchor_raw);
    while (!anchor.empty() && anchor[0] == '#')
      anchor.erase(anchor.begin());
    if (anchor.empty())
      return "";
    if (anchor.size() > 512)
      anchor.resize(512);

    std::string key = NormalizePathForAnchor(doc);
    if (key.empty())
      return "";
    key.push_back('#');
    key += anchor;
    return key;
  }

  // Converts a TOC href to a normalized key for chapter anchor lookup, handling URL decoding and stripping of fragments/queries as needed.
  static std::string NormalizeAnchorHrefKey(const std::string &href)
  {
    if (href.empty())
      return "";
    std::string decoded = UrlDecodeComponent(href);
    size_t hash = decoded.find('#');
    if (hash == std::string::npos || hash + 1 >= decoded.size())
      return "";
    std::string path = decoded.substr(0, hash);
    std::string anchor = decoded.substr(hash + 1);
    size_t q = anchor.find('?');
    if (q != std::string::npos)
      anchor = anchor.substr(0, q);
    return BuildAnchorKey(path, anchor);
  }

  // Normalizes a document path for use as a key in chapter doc-start page lookup, optionally stripping fragments and queries.
  static std::string NormalizeDocStartPathKey(const std::string &raw_path,
                                              bool strip_fragment_and_query)
  {
    if (raw_path.empty())
      return "";

    std::string decoded = UrlDecodeComponent(raw_path);
    if (strip_fragment_and_query)
    {
      size_t hash = decoded.find('#');
      if (hash != std::string::npos)
        decoded = decoded.substr(0, hash);
      size_t q = decoded.find('?');
      if (q != std::string::npos)
        decoded = decoded.substr(0, q);
    }

    return NormalizePathForAnchor(decoded);
  }

} // namespace

Book::Book(const BookContext &c) : ctx(c)
{
  // State / Formats
  mupdf_state = NULL;
  cbz_state = NULL;
  reflow_worker_state = NULL;
  inline_image_probe_uf = NULL;
  format = FORMAT_UNDEF;

  // Position state / basic rendering
  position = 0;
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
  mobi_line_wrap_fix = false;
  parsed_with_mobi_line_wrap_fix = false;
  browser_display_name_cached = false;
  inline_image_zip_index_built = false;
  mobi_inline_index_ready = false;
  mobi_first_image_index = 0;
  fb2_inline_images_bytes = 0;
  inline_image_cache_bytes = 0;
  layout_revision = 0;

  // Session state
  open_session_id_ = 0;
  open_abort_requested_ = false;
  ClearTocConfidence();
}

// Destructor for the Book class, responsible for cleaning up resources and closing the book if it's still open. Also logs the book closure if a status reporter is available.
Book::~Book()
{
#ifdef DSLIBRIS_DEBUG
  if (GetStatusReporter())
  {
    DBG_LOGF(GetStatusReporter(), "BOOK ~Book: path=%s/%s",
             foldername.c_str(), filename.c_str());
  }
#endif
  Close();
  if (coverPixels)
  {
    delete[] coverPixels;
    coverPixels = nullptr;
  }
}

IStatusReporter *Book::GetStatusReporter() { return ctx.status_reporter; }

Text *Book::GetText() { return ctx.text; }

Prefs *Book::GetPrefs() { return ctx.prefs; }

int Book::GetParagraphSpacing()
{
  return ctx.paragraph_spacing ? *ctx.paragraph_spacing : 0;
}

int Book::GetParagraphIndent()
{
  return ctx.paragraph_indent ? *ctx.paragraph_indent : 0;
}

int Book::GetOrientation() { return ctx.orientation ? *ctx.orientation : 0; }

void Book::DrawBottomGradientBackground()
{
  if (ctx.draw_background)
    ctx.draw_background(ctx.draw_background_user_data);
}

void Book::SetFolderName(const char *name) { foldername = name; }

void Book::SetFileName(const char *name)
{
  filename = name;
  ClearBrowserDisplayNameCache();
}

void Book::SetTitle(const char *name)
{
  title = name;
  ClearBrowserDisplayNameCache();
}

void Book::SetAuthor(const std::string &name) { author = name; }

void Book::SetFolderName(const std::string &name) { foldername = name; }

std::list<u16> &Book::GetBookmarks() { return bookmarks; }

const std::list<u16> &Book::GetBookmarks() const { return bookmarks; }
const std::vector<ChapterEntry> &Book::GetChapters() const { return chapters; }
// TODO: If more formats/endpoints reuse chapter href normalization logic,
// consider moving the path/anchor normalization helpers into a small shared module. 
void Book::AddChapterAnchor(const std::string &docpath,
                            const std::string &anchor_id)
{
  if (docpath.empty() || anchor_id.empty())
    return;
  if (chapter_anchor_pages.size() >= 8192)
    return;

  std::string key = BuildAnchorKey(docpath, anchor_id);
  if (key.empty())
    return;

  if (chapter_anchor_pages.find(key) == chapter_anchor_pages.end())
  {
    chapter_anchor_pages[key] = GetPageCount();
  }
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
bool Book::FindChapterAnchorPage(const std::string &href, u16 *page_out) const
{
  if (!page_out)
    return false;
  std::string key = NormalizeAnchorHrefKey(href);
  if (key.empty())
    return false;

  auto hit = chapter_anchor_pages.find(key);
  if (hit != chapter_anchor_pages.end())
  {
    *page_out = hit->second;
    return true;
  }

  // Fallback only for malformed files with inconsistent anchor case.
  std::string key_lc = ToLowerAsciiLocal(key);
  for (const auto &kv : chapter_anchor_pages)
  {
    if (ToLowerAsciiLocal(kv.first) == key_lc)
    {
      *page_out = kv.second;
      return true;
    }
  }

  // Robust fallback for malformed EPUBs where TOC path and parsed doc path
  // differ but anchor IDs are still consistent.
  size_t hash = key.find('#');
  if (hash != std::string::npos && hash + 1 < key.size())
  {
    std::string key_path_lc = ToLowerAsciiLocal(key.substr(0, hash));
    std::string key_base_lc = ToLowerAsciiLocal(BasenamePathLocal(key_path_lc));
    std::string key_anchor_lc = ToLowerAsciiLocal(key.substr(hash + 1));
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
    const std::string key_token = AnchorTokenKey(key_anchor_lc);
    const std::string key_digits = AnchorDigits(key_anchor_lc);

    for (const auto &kv : chapter_anchor_pages)
    {
      size_t kv_hash = kv.first.find('#');
      if (kv_hash == std::string::npos || kv_hash + 1 >= kv.first.size())
        continue;
      std::string kv_path_lc = ToLowerAsciiLocal(kv.first.substr(0, kv_hash));
      std::string kv_base_lc = ToLowerAsciiLocal(BasenamePathLocal(kv_path_lc));
      std::string kv_anchor_lc =
          ToLowerAsciiLocal(kv.first.substr(kv_hash + 1));
      bool exact_anchor = (kv_anchor_lc == key_anchor_lc);

      if (exact_anchor)
      {
        if (!anchor_found)
        {
          anchor_found = true;
          anchor_page = kv.second;
        }
        else if (anchor_page != kv.second)
        {
          anchor_ambiguous = true;
        }

        if (has_target_doc)
        {
          u16 candidate_doc_page = 0;
          if (FindChapterDocStartPage(kv.first, &candidate_doc_page) &&
              candidate_doc_page == target_doc_page)
          {
            if (!path_anchor_found)
            {
              path_anchor_found = true;
              path_anchor_page = kv.second;
            }
            else if (path_anchor_page != kv.second)
            {
              path_anchor_ambiguous = true;
            }
          }
        }

        if (!key_base_lc.empty() && kv_base_lc == key_base_lc)
        {
          if (!base_anchor_found)
          {
            base_anchor_found = true;
            base_anchor_page = kv.second;
          }
          else if (base_anchor_page != kv.second)
          {
            base_anchor_ambiguous = true;
          }
        }
      }

      if (has_target_doc && !path_anchor_found && !key_token.empty())
      {
        u16 candidate_doc_page = 0;
        if (FindChapterDocStartPage(kv.first, &candidate_doc_page) &&
            candidate_doc_page == target_doc_page)
        {
          std::string cand_token = AnchorTokenKey(kv_anchor_lc);
          std::string cand_digits = AnchorDigits(kv_anchor_lc);
          bool fuzzy_match = false;
          if (!cand_token.empty() && cand_token == key_token)
          {
            fuzzy_match = true;
          }
          else if (!key_digits.empty() && !cand_digits.empty() &&
                   cand_digits == key_digits)
          {
            fuzzy_match = true;
          }
          else if (!key_digits.empty() && !cand_token.empty() &&
                   cand_token.size() > key_digits.size() &&
                   cand_token.size() <= key_digits.size() + 4 &&
                   cand_token.compare(cand_token.size() - key_digits.size(),
                                      key_digits.size(), key_digits) == 0)
          {
            fuzzy_match = true;
          }

          if (fuzzy_match)
          {
            if (!fuzzy_doc_found)
            {
              fuzzy_doc_found = true;
              fuzzy_doc_page = kv.second;
            }
            else if (fuzzy_doc_page != kv.second)
            {
              fuzzy_doc_ambiguous = true;
            }
          }
        }
      }
    }

    if (path_anchor_found && !path_anchor_ambiguous)
    {
      *page_out = path_anchor_page;
      return true;
    }
    if (fuzzy_doc_found && !fuzzy_doc_ambiguous)
    {
      *page_out = fuzzy_doc_page;
      return true;
    }
    if (base_anchor_found && !base_anchor_ambiguous)
    {
      *page_out = base_anchor_page;
      return true;
    }
    if (anchor_found && !anchor_ambiguous)
    {
      *page_out = anchor_page;
      return true;
    }
  }

  return false;
}

size_t Book::GetChapterAnchorCount() const
{
  return chapter_anchor_pages.size();
}

void Book::ClearChapterAnchors() { chapter_anchor_pages.clear(); }

void Book::SetChapterDocStartPage(const std::string &docpath, u16 page)
{
  if (docpath.empty())
    return;
  std::string key = NormalizeDocStartPathKey(docpath, false);
  if (key.empty())
    return;
  if (chapter_doc_start_pages.find(key) == chapter_doc_start_pages.end())
    chapter_doc_start_pages[key] = page;
}

bool Book::FindChapterDocStartPage(const std::string &href,
                                   u16 *page_out) const
{
  if (!page_out)
    return false;
  std::string key = NormalizeDocStartPathKey(href, true);
  if (key.empty())
    return false;

  auto hit = chapter_doc_start_pages.find(key);
  if (hit != chapter_doc_start_pages.end())
  {
    *page_out = hit->second;
    return true;
  }

  std::string key_lc = ToLowerAsciiLocal(key);
  for (const auto &kv : chapter_doc_start_pages)
  {
    if (ToLowerAsciiLocal(kv.first) == key_lc)
    {
      *page_out = kv.second;
      return true;
    }
  }

  return false;
}

const std::unordered_map<std::string, u16> &
Book::GetChapterDocStartPages() const
{
  return chapter_doc_start_pages;
}

void Book::ClearChapterDocStartPages() { chapter_doc_start_pages.clear(); }

void Book::AddChapter(u16 page, const std::string &title, u8 level)
{
  ChapterEntry entry;
  entry.page = page;
  entry.level = level;
  entry.title = title;
  chapters.push_back(entry);
}

void Book::ClearChapters() { chapters.clear(); }

bool Book::IsPdf() const { return format == FORMAT_PDF; }

bool Book::IsCbz() const { return format == FORMAT_CBZ; }

bool Book::IsFixedLayout() const { return IsPdf() || IsCbz(); }

const char *Book::GetFixedLayoutLabel() const
{
  if (IsCbz())
    return "CBZ";
  if (IsPdf() && mupdf_state)
  {
    return app_flow_utils::GetMuPdfDocumentLabel(mupdf_state->document_kind);
  }
  if (IsPdf())
    return "PDF";
  return "BOOK";
}

bool Book::UsesTextLayoutSettings() const { return !IsFixedLayout(); }

bool Book::SupportsBookmarks() const { return !IsFixedLayout(); }

Page *Book::GetPage() { return pages[position]; }

Page *Book::GetPage(int index) { return pages[index]; }

u16 Book::GetPageCount()
{
  if (IsPdf() && mupdf_state)
    return mupdf_state->page_count;
  if (IsCbz() && cbz_state)
    return cbz_state->page_count;
  return pages.size();
}

const char *Book::GetTitle() { return title.c_str(); }

const char *Book::GetFileName() { return filename.c_str(); }

const char *Book::GetFolderName() { return foldername.c_str(); }

int Book::GetPosition() { return position; }

void Book::SetPage(u16 index) { position = index; }

void Book::SetPosition(int pos) { position = pos; }

Page *Book::AppendPage()
{
  Page *page = new Page(this);
  pages.push_back(page);
  return page;
}

void Book::ReservePageCapacity(size_t incoming_pages)
{
  const size_t required_capacity = page_buffer_utils::RequiredPageVectorCapacity(
      pages.size(), pages.capacity(), incoming_pages);
  if (required_capacity > pages.capacity())
    pages.reserve(required_capacity);
}

void Book::Close()
{
  IStatusReporter *r = GetStatusReporter();
  DBG_LOGF(r, "BOOK close: begin book=%s", filename.c_str());
  const bool flush_pending_epub_cache =
      reflow_cache_save_utils::ShouldFlushDeferredCacheSaveOnClose(
          epub_page_cache_save_pending, IsAsyncReflowOpenPending(),
          (unsigned int)GetPageCount());
  DBG_LOGF(r, "BOOK close: cancel-async-reflow book=%s", filename.c_str());
  CancelAsyncReflowOpen();
  if (flush_pending_epub_cache)
  {
    DBG_LOGF(r, "BOOK close: save-epub-cache begin pages=%d book=%s", (int)pages.size(), filename.c_str());
    epub_page_cache::SavePending(this, true);
    DBG_LOGF(r, "BOOK close: save-epub-cache done book=%s", filename.c_str());
  }
  epub_page_cache_save_pending = false;
  DBG_LOGF(r, "BOOK close: cancel-mobi-parse book=%s", filename.c_str());
  CancelDeferredMobiParse();
  DBG_LOGF(r, "BOOK close: clear-pages count=%d book=%s", (int)pages.size(), filename.c_str());
  std::vector<Page *>::iterator it = pages.begin();
  while (it != pages.end())
  {
    delete *it;
    *it = nullptr;
    ++it;
  }
  pages.clear();
  DBG_LOGF(r, "BOOK close: reset-reflow book=%s", filename.c_str());
  ResetReflowWorkerState();
  DBG_LOGF(r, "BOOK close: reset-cbz book=%s", filename.c_str());
  ResetCbzState();
  DBG_LOGF(r, "BOOK close: reset-mupdf book=%s", filename.c_str());
  ResetMuPdfState();
  chapters.clear();
  ClearChapterAnchors();
  ClearChapterDocStartPages();
  ClearInlineImages();
  ClearTocConfidence();
  open_session_id_ = 0;
  open_abort_requested_ = false;
  DBG_LOGF(r, "BOOK close: done book=%s", filename.c_str());
}

void Book::ResetCbzFailureState()
{
  if (!IsCbz() || !cbz_state)
    return;
  cbz_state->failed_page = -1;
  cbz_state->logged_failed_page = -1;
  cbz_state->last_error.clear();
}

bool Book::IsMobiFile() const { return HasExtCaseInsensitive(filename, ".mobi"); }

bool Book::GetMobiLineWrapFix() const { return mobi_line_wrap_fix; }

void Book::SetMobiLineWrapFix(bool enabled) { mobi_line_wrap_fix = enabled; }

void Book::MarkMobiRenderSettingsApplied(bool enabled)
{
  parsed_with_mobi_line_wrap_fix = enabled;
}

bool Book::NeedsMobiRenderRefresh() const
{
  return IsMobiFile() && parsed_with_mobi_line_wrap_fix != mobi_line_wrap_fix;
}

bool Book::HasPendingEpubPageCacheSave() const
{
  return epub_page_cache_save_pending;
}

void Book::SetPendingEpubPageCacheSave(bool pending)
{
  epub_page_cache_save_pending = pending;
}

void Book::SetPendingEpubPageCacheSaveWithParams(
    int pixel_size, int line_spacing, int paragraph_spacing,
    int paragraph_indent, int orientation,
    int margin_left, int margin_right, int margin_top, int margin_bottom,
    const char *regular_font)
{
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

const Book::EpubCacheSaveParams &Book::GetEpubCacheSaveParams() const
{
  return epub_cache_save_params;
}

unsigned int Book::GetLayoutRevision() const { return layout_revision; }

void Book::SetLayoutRevision(unsigned int revision)
{
  layout_revision = revision;
}

unsigned int Book::GetOpenSessionId() const { return open_session_id_; }

void Book::SetOpenSessionId(unsigned int session_id)
{
  open_session_id_ = session_id;
}

bool Book::IsOpenAbortRequested() const { return open_abort_requested_; }

void Book::RequestAbortOpen() { open_abort_requested_ = true; }

void Book::ClearOpenAbortRequest() { open_abort_requested_ = false; }

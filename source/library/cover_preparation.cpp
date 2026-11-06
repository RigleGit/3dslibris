#include "library/cover_preparation.h"
#include "library/cover_cache.h"
#include "library/browser_warmup_utils.h"
#include "book/book.h"
#include "formats/common/book_error.h"
#include "formats/epub/epub_parser.h"
#include "formats/fb2/fb2_parser.h"
#include "formats/mobi/mobi_parser.h"
#include "formats/pdf/pdf_parser.h"
#include "formats/cbz/cbz_parser.h"
#include "shared/home_button_guard.h"
#include "shared/string_utils.h"
#include <3ds.h>
namespace library_cover {
Result Prepare(Book *book,const std::string &path,bool is_new,bool selected,bool cache_only) {
  if(!book || path.empty()) return {Status::Absent,0,false};
  if(cover_cache::TryLoadCachedOnly(book,path)) return {Status::Cached,0,false};
  if(!book->IsBrowserFolder() && cover_cache::IsMarkedAbsent(book,path)) {
    book->coverAttempts=cover_cache::kMaxAttempts;
    return {Status::Absent,0,false};
  }
  const bool adjacent=cover_cache::HasAdjacentOverride(book,path);
  if(!adjacent) {
    if(book->IsBrowserFolder()) return {Status::Absent,0,false};
    if(book->format==FORMAT_EPUB) {
      if(!book->metadataIndexTried) return {Status::NeedsMetadata,0,false};
      if(book->coverImagePath.empty()) {
        book->coverAttempts=cover_cache::kMaxAttempts;
        return {Status::Absent,0,false};
      }
    } else if(book->format!=FORMAT_PDF && book->format!=FORMAT_CBZ &&
              !(book->format==FORMAT_XHTML &&
                (HasExtCI(book->GetFileName(),".fb2") || HasExtCI(book->GetFileName(),".mobi")))) {
      book->coverAttempts=cover_cache::kMaxAttempts;
      return {Status::Unsupported,0,false};
    }
  }
  if(!browser_warmup_utils::HasCoverExtractionHeadroom(is_new,selected,osGetMemRegionFree(MEMREGION_ALL)))
    return {Status::LowMemory,4,false};
  if(cache_only) return {Status::NeedsPreparation,0,false};
  bool loaded=false;int rc=0;bool supported=true;
  if(adjacent) loaded=cover_cache::TryLoadAdjacentOverride(book,path);
  if(!loaded) {
    if(book->IsBrowserFolder()) {rc=adjacent ? 1 : 0;supported=false;}
    else if(book->format==FORMAT_EPUB) {
      if(!book->metadataIndexTried) return {Status::NeedsMetadata,0,false};
      if(book->coverImagePath.empty()) {
        book->coverAttempts=cover_cache::kMaxAttempts;
        return {adjacent ? Status::Error : Status::Absent,adjacent ? 1 : 0,adjacent};
      }
      HomeButtonGuard guard;rc=epub_parser::ExtractCover(book,path);
    } else if(book->format==FORMAT_XHTML && HasExtCI(book->GetFileName(),".fb2")) {
      HomeButtonGuard guard;rc=fb2_parser::ExtractCover(book,path);
    } else if(book->format==FORMAT_XHTML && HasExtCI(book->GetFileName(),".mobi")) {
      HomeButtonGuard guard;rc=mobi_parser::ExtractCover(book,path);
    } else if(book->format==FORMAT_PDF) {
      HomeButtonGuard guard;rc=pdf_parser::ExtractCover(book,path);
    } else if(book->format==FORMAT_CBZ) {
      HomeButtonGuard guard;rc=cbz_parser::ExtractCover(book,path);
    } else {supported=false;rc=adjacent ? 1 : 0;}
  }
  if(rc==BOOK_ERR_CANCELLED) return {Status::Cancelled,rc,true};
  if((loaded || rc==0) && book->coverPixels) {
    book->coverAttempts=cover_cache::kMaxAttempts;book->coverRetryAfterMs=0;
    return {cover_cache::Save(book,path) ? Status::Prepared : Status::WriteError,0,true};
  }
  const bool fb2=book->format==FORMAT_XHTML && HasExtCI(book->GetFileName(),".fb2");
  if(fb2 && !adjacent && !book->IsBrowserFolder() && browser_warmup_utils::IsDefinitiveFb2NoCover(rc)) {
    book->coverAttempts=cover_cache::kMaxAttempts;
    cover_cache::MarkAbsent(book,path);
    return {Status::Absent,0,true};
  }
  if(rc!=0) {
    if(book->IsBrowserFolder() || (book->format==FORMAT_PDF && browser_warmup_utils::IsPermanentCoverFailure(rc))) book->coverAttempts=cover_cache::kMaxAttempts;
    else ++book->coverAttempts;
    return {Status::Error,rc,true};
  }
  book->coverAttempts=cover_cache::kMaxAttempts;
  // The extractor ran cleanly and found nothing: remember that for this exact
  // source so later sessions do not reopen the document. Errors never qualify.
  if(supported && !adjacent && !book->IsBrowserFolder()) cover_cache::MarkAbsent(book,path);
  return {supported || book->IsBrowserFolder() ? Status::Absent : Status::Unsupported,0,supported || adjacent};
}
}

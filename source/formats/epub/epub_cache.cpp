/*

dslibris - an ebook reader for the Nintendo DS.

 Copyright (C) 2007-2008 Ray Haleblian

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

*/

#include "formats/epub/epub_cache.h"

#include "book/reflow_cache_save_utils.h"
#include "debug_log.h"
#include "formats/epub/epub_manifest.h"
#include "formats/epub/epub_page_cache.h"
#include "shared/status_reporter.h"
#include <3ds.h>

typedef BookParseDeps EpubDeps;

int FinalizeEpubParse(unzFile uf, epub_data_t *parsedata, Book *book,
                      const std::string &name, const EpubDeps &deps,
                      int rc, bool save_cache) {
  if (save_cache) {
    if (reflow_cache_save_utils::ShouldDeferAsyncOpenCacheSave(
            true, book && book->IsAsyncReflowOpenPending())) {
      book->SetPendingEpubPageCacheSave(true);
    } else {
      epub_page_cache::Save(book, name.c_str(),
                            deps.ts ? (int)deps.ts->GetPixelSize() : 0,
                            deps.ts ? (int)deps.ts->linespacing : 0,
                            deps.paragraph_spacing, deps.paragraph_indent,
                            deps.orientation,
                            deps.ts ? (int)deps.ts->margin.left : 0,
                            deps.ts ? (int)deps.ts->margin.right : 0,
                            deps.ts ? (int)deps.ts->margin.top : 0,
                            deps.ts ? (int)deps.ts->margin.bottom : 0,
                            deps.ts ? deps.ts->GetFontFile(TEXT_STYLE_REGULAR).c_str() : NULL);
      if (book)
        book->SetPendingEpubPageCacheSave(false);
    }
  }
  if (uf)
    unzClose(uf);
  if (parsedata)
    epub_data_delete(parsedata);
  if (deps.reporter)
    DBG_LOG(deps.reporter, "EPUB: parse end");
  return rc;
}

#pragma once

#include "book/book_parse_deps.h"
#include "formats/common/plain_text_stream.h"
#include "formats/mobi/mobi_toc_finalize.h"
#include "formats/mobi/mobi_toc_prepare.h"

namespace mobi_parser {

typedef std::string (*ExtractMarkupToTextFn)(
    Book *book, const BookParseDeps &deps, const std::string &in,
    std::vector<mobi_toc_finalize::MobiHeadingHint> *heading_hints,
    std::vector<std::pair<u32, u32>> *html_to_text_map);

typedef mobi_toc_prepare::StructuredCallbacks (*MakeStructuredTocCallbacksFn)();
typedef mobi_toc_prepare::InlineTitleCallbacks (*MakeInlineTitleCallbacksFn)();
typedef mobi_toc_finalize::FinalizeCallbacks (*MakeFinalizeCallbacksFn)();
typedef plain_text_stream::ContinueCallbacks (*MakePlainContinueCallbacksFn)();

struct Hooks {
  ExtractMarkupToTextFn extract_markup_to_text;
  MakeStructuredTocCallbacksFn make_structured_toc_callbacks;
  MakeInlineTitleCallbacksFn make_inline_title_callbacks;
  MakeFinalizeCallbacksFn make_finalize_callbacks;
  MakePlainContinueCallbacksFn make_plain_continue_callbacks;

  Hooks()
      : extract_markup_to_text(NULL), make_structured_toc_callbacks(NULL),
        make_inline_title_callbacks(NULL), make_finalize_callbacks(NULL),
        make_plain_continue_callbacks(NULL) {}
};

u8 ParseFile(Book *book, const char *path, const Hooks &hooks);

} // namespace mobi_parser

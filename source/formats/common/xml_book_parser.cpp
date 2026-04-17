#include "formats/common/xml_book_parser.h"

#include "book/book.h"
#include "book/book_xml.h"
#include "formats/common/plain_text_perf_utils.h"
#include "formats/common/book_error.h"
#include "formats/common/xml_parse_utils.h"
#include "parse.h"
#include "shared/parser_limits.h"
#include "shared/string_utils.h"

#include <stdio.h>

namespace xml_book_parser {
namespace {

using plain_text_perf_utils::CapturePlainTextPerfBaseline;
using plain_text_perf_utils::FillPlainTextStreamPerf;
using plain_text_perf_utils::LogPlainTextStreamPerf;
using plain_text_perf_utils::PlainTextPerfBaseline;
using plain_text_perf_utils::PlainTextStreamPerf;

static void InitParsedataWithDeps(parsedata_t *parsedata, Book *book,
                                  const BookParseDeps &deps) {
  if (!parsedata)
    return;
  parse_init(parsedata);
  parsedata->reporter = deps.reporter;
  parsedata->ts = deps.ts;
  parsedata->prefs = deps.prefs;
  parsedata->book = book;
}

} // namespace

u8 ParseXmlBookFile(Book *book, const char *path, bool fulltext,
                    const BookParseDeps &deps,
                    BuildFb2FallbackChaptersFn build_fb2_fallback,
                    SetTocConfidenceFn set_toc_confidence) {
  if (!book || !path)
    return 255;
  FILE *fp = fopen(path, "r");
  if (!fp)
    return 255;

  u8 rc = 0;
  parsedata_t parsedata;
  InitParsedataWithDeps(&parsedata, book, deps);
  parsedata.fb2_mode = fulltext && HasExtCI(book->GetFileName(), ".fb2");
#ifdef DSLIBRIS_DEBUG
  const u64 xml_parse_begin = osGetTime();
  const u16 xml_pages_before = book->GetPageCount();
#endif
  PlainTextPerfBaseline xml_perf_baseline;
  CapturePlainTextPerfBaseline(parsedata, &xml_perf_baseline);

  xml_parse_utils::XmlParserOptions options;
  options.user_data = &parsedata;
  options.abort_parse = [](void *user_data) {
    parsedata_t *parsedata = static_cast<parsedata_t *>(user_data);
    return parsedata &&
           ((parsedata->book && parsedata->book->IsOpenAbortRequested()) ||
            (parsedata->reporter && parsedata->reporter->ShouldAbortWork()));
  };
  options.abort_user_data = &parsedata;
  options.default_handler = xml::book::fallback;
  options.processing_instruction = xml::book::instruction;
  options.start_element = xml::book::start;
  options.end_element = xml::book::end;
  options.character_data = xml::book::chardata;
  if (!fulltext) {
    options.start_element = xml::book::metadata::start;
    options.end_element = xml::book::metadata::end;
    options.character_data = xml::book::metadata::chardata;
  }
  xml_parse_utils::XmlParseResult parse_result =
      xml_parse_utils::ParseXmlFileStream(
          fp, options, parser_limits::kXmlStreamBufferSize,
          [](void *user_data) {
            parsedata_t *parsedata = static_cast<parsedata_t *>(user_data);
            return parsedata && parsedata->status;
          });
  fclose(fp);
  if (!parse_result.ok) {
    if (parse_result.error_code == XML_ERROR_ABORTED) {
      rc = BOOK_ERR_CANCELLED;
    } else {
      if (deps.reporter)
        deps.reporter->PrintStatus(
            xml_parse_utils::FormatXmlParseError(parse_result).c_str());
      rc = 254;
    }
  }

  if (rc == 0 && fulltext && parsedata.fb2_mode) {
    bool has_structured_toc = !book->GetChapters().empty();
    if (!has_structured_toc && build_fb2_fallback)
      build_fb2_fallback(book);
    if (!book->GetChapters().empty()) {
      if (set_toc_confidence)
        set_toc_confidence(book, has_structured_toc);
    } else {
      book->ClearTocConfidence();
    }
  }

#ifdef DSLIBRIS_DEBUG
  if (deps.reporter && fulltext) {
    PlainTextStreamPerf perf;
    FillPlainTextStreamPerf(parsedata, xml_perf_baseline,
                            osGetTime() - xml_parse_begin, 0,
                            book->GetPageCount() - xml_pages_before, &perf);
    LogPlainTextStreamPerf(
        deps.reporter, parsedata.fb2_mode ? "FB2 layout" : "XML layout", perf,
        rc == 0);
  }
#endif

  return rc;
}

} // namespace xml_book_parser

#pragma once

#include "formats/common/xml_book_parser.h"
#include "formats/epub/epub_stylesheet_utils.h"
#include "shared/orientation_utils.h"

#include <unistd.h>

namespace {

// A buffer-only continuity assertion misses text abandoned by Page::Draw
// when the paginator has placed more lines on a screen than it can display.
void TestXmlPageRenderingContinuity() {
  for (unsigned char orientation = 0; orientation < 3; orientation++) {
    for (int pixel_size : {12, 14, 20, 24}) {
      for (int line_spacing : {0, 2, 6}) {
        for (bool coalesced : {false, true}) {
          TestCtx tc;
          tc.ctx.orientation = &orientation;
          tc.paragraph_spacing = 0;
          tc.text.SetPixelSize((u8)pixel_size);
          tc.text.linespacing = line_spacing;
          tc.text.display.width = 240;
          tc.text.landscape = orientation_utils::IsLandscape(orientation);
          tc.text.capture_rendered_text = true;
          u16 left = 0, right = 0;
          tc.text.screenleft = &left;
          tc.text.screenright = &right;
          Book book(tc.ctx);

          std::string expected;
          std::string html = "<html><body>";
          for (int paragraph = 0; paragraph < 10; paragraph++) {
            html += "<p>";
            for (int word = 0; word < 150; word++) {
              char token[32];
              snprintf(token, sizeof(token), "w%02d%03d", paragraph, word);
              expected += token;
              html += token;
              html += ' ';
            }
            html += "</p>";
          }
          html += "</body></html>";

          if (coalesced) {
            // EPUB combines text callbacks. Start with a valid baseline here
            // to isolate the screen-metrics regression from initialization.
            parsedata_t p = MakeParseData(tc, book);
            p.base_font_size_px = (u8)pixel_size;
            p.pen.y = tc.text.margin.top + tc.text.GetHeight();
            p.coalesce_text_segments = true;
            const xml_parse_utils::XmlParseResult result =
                xml_parse_utils::ParseXmlString(html, MakeXmlOpts(&p));
            ExpectTrue("coalesced XML parses", result.ok);
          } else {
            // Exercise production initialization and streamed XML callbacks,
            // the same entry point used by FB2.
            char filename[] = "/tmp/3dslibris-render-XXXXXX";
            const int fd = mkstemp(filename);
            ExpectTrue("render fixture created", fd >= 0);
            FILE *file = fdopen(fd, "wb");
            ExpectTrue("render fixture opened", file != nullptr);
            const size_t written = fwrite(html.data(), 1, html.size(), file);
            fclose(file);
            ExpectTrue("render fixture written", written == html.size());
            const u8 result = xml_book_parser::ParseXmlBookFile(
                &book, filename, true, BuildBookParseDeps(&book), nullptr,
                nullptr);
            unlink(filename);
            ExpectIntEq("streamed XML parses", result, 0);
          }

          ExpectTrue("render fixture spans pages", book.GetPageCount() > 1);
          for (int page = 0; page < book.GetPageCount(); page++)
            book.GetPage(page)->Draw(&tc.text);
          if (tc.text.rendered_ascii != expected) {
            fprintf(stderr,
                    "render continuity: orientation=%u px=%d spacing=%d "
                    "coalesced=%d expected=%zu drawn=%zu clipped=%d\n",
                    orientation, pixel_size, line_spacing, coalesced,
                    expected.size(), tc.text.rendered_ascii.size(),
                    tc.text.clipped_glyphs);
          }
          ExpectTrue("every word is drawn once and in order",
                     tc.text.rendered_ascii == expected);
          ExpectIntEq("no glyph falls outside the reading area",
                      tc.text.clipped_glyphs, 0);
        }
      }
    }
  }
}

void TestHrAtPageEdgeKeepsFollowingHeading() {
  TestCtx tc;
  tc.paragraph_spacing = 0;
  tc.text.SetPixelSize(12);
  tc.text.capture_rendered_text = true;
  u16 left = 0, right = 0;
  tc.text.screenleft = &left;
  tc.text.screenright = &right;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  p.pen.y = tc.text.margin.top + tc.text.GetHeight();
  p.coalesce_text_segments = true;

  std::string html = "<html><body>";
  std::string expected;
  for (int n = 0; n < 24; n++) {
    char token[16];
    snprintf(token, sizeof(token), "W%03d", n);
    html += "<p>";
    html += token;
    html += "</p>";
    expected += token;
  }
  html += "<hr/><p style='text-align:center'><strong>HEADER2013</strong>"
          "<br/><small>Subtitle</small></p><p>AFTERWORD</p></body></html>";
  expected += "HEADER2013SubtitleAFTERWORD";

  ExpectTrue("HR page-edge fixture parses",
             xml_parse_utils::ParseXmlString(html, MakeXmlOpts(&p)).ok);
  for (int page = 0; page < book.GetPageCount(); page++)
    book.GetPage(page)->Draw(&tc.text);
  ExpectTrue("HR page-edge heading and later text are drawn",
             tc.text.rendered_ascii == expected);
  ExpectIntEq("HR page-edge does not clip glyphs", tc.text.clipped_glyphs, 0);
}


void ExpectAlignedGlyphLines(Text &text, int alignment) {
  const std::vector<Text::RenderedGlyph> &glyphs = text.rendered_glyphs;
  ExpectTrue("aligned text was drawn", !glyphs.empty());
  for (size_t start = 0; start < glyphs.size();) {
    size_t end = start + 1;
    while (end < glyphs.size() && glyphs[end].y == glyphs[start].y &&
           glyphs[end].screen == glyphs[start].screen)
      end++;
    const int width = (int)(end - start) * text.GetAdvance('A');
    const int available = text.LogicalWidthFor(
        glyphs[start].screen == text.screenleft) - text.margin.left -
        text.margin.right;
    const int offset = alignment == 1 ? (available - width) / 2
                                     : available - width;
    ExpectIntEq("every visual line uses its paragraph alignment",
                glyphs[start].x, text.margin.left + std::max(0, offset));
    start = end;
  }
}

void TestEmbeddedCssParagraphSpacing() {
  for (int margin_em : {1, 2}) {
    for (bool vertical : {false, true}) {
      for (bool horizontal : {false, true}) {
        TestCtx tc;
        tc.paragraph_spacing = 0;
        tc.publisher_block_margins = vertical;
        tc.publisher_horizontal_margins = horizontal;
        tc.text.capture_rendered_text = true;
        u16 left = 0, right = 0;
        tc.text.screenleft = &left;
        tc.text.screenright = &right;
        Book book(tc.ctx);
        parsedata_t p = MakeParseData(tc, book);
        p.pen.y = tc.text.margin.top + tc.text.GetHeight();
        const std::string html = "<html><head><style>p{margin-top:" +
            std::to_string(margin_em) + "em;margin-bottom:" +
            std::to_string(margin_em) + "em;margin-left:2em;margin-right:2em}</style>"
            "</head><body><p>A</p><p>B</p></body></html>";
        for (const auto &sheet : epub_stylesheet_utils::ExtractHeadStylesheets(html))
          epub_css_class_map::ParseCssIntoClassMap(sheet.css.data(), sheet.css.size(),
                                                  &p.css_class_map);
        ExpectTrue("spacing XML parses",
                   xml_parse_utils::ParseXmlString(html, MakeXmlOpts(&p)).ok);
        ExpectIntEq("spacing stays on one page", book.GetPageCount(), 1);
        book.GetPage(0)->Draw(&tc.text);
        int ay = -1, by = -1;
        for (const auto &glyph : tc.text.rendered_glyphs) {
          if (glyph.codepoint == 'A') ay = glyph.y;
          if (glyph.codepoint == 'B') by = glyph.y;
        }
        ExpectTrue("both paragraphs drawn", ay >= 0 && by > ay);
        const int line_step = tc.text.GetHeight() + tc.text.linespacing;
        ExpectIntEq("CSS gap follows publisher spacing independently of sides",
                    by - ay, (vertical ? margin_em + 1 : 2) * line_step);
      }
    }
  }
}

void TestAdjacentDivsStartSeparateLines() {
  TestCtx tc;
  tc.paragraph_spacing = 0;
  tc.publisher_block_margins = true;
  tc.text.capture_rendered_text = true;
  u16 left = 0, right = 0;
  tc.text.screenleft = &left;
  tc.text.screenright = &right;
  Book book(tc.ctx);
  parsedata_t p = MakeParseData(tc, book);
  p.pen.y = tc.text.margin.top + tc.text.GetHeight();
  p.coalesce_text_segments = true;
  const std::string html =
      "<html><head><style>.toc-title{display:block;margin-bottom:0}"
      ".toc-entry{display:block;margin-top:0}</style></head><body>"
      "<div class='toc-title'>Table of Contents</div>"
      "<div class='toc-entry'>Title Page</div></body></html>";
  for (const auto &sheet : epub_stylesheet_utils::ExtractHeadStylesheets(html))
    epub_css_class_map::ParseCssIntoClassMap(sheet.css.data(), sheet.css.size(),
                                            &p.css_class_map);
  ExpectTrue("adjacent divs parse",
             xml_parse_utils::ParseXmlString(html, MakeXmlOpts(&p)).ok);
  ExpectIntEq("adjacent divs stay on one page", book.GetPageCount(), 1);
  book.GetPage(0)->Draw(&tc.text);
  int first_y = -1, second_y = -1;
  for (const auto &glyph : tc.text.rendered_glyphs) {
    if (glyph.codepoint != 'T')
      continue;
    if (first_y < 0)
      first_y = glyph.y;
    else {
      second_y = glyph.y;
      break;
    }
  }
  ExpectTrue("both divs rendered", first_y >= 0 && second_y >= 0);
  ExpectTrue("adjacent divs begin on separate lines", second_y > first_y);
}

void TestEmbeddedCssAlignedLines() {
  for (int alignment : {1, 2}) {
    TestCtx tc;
    tc.paragraph_spacing = 0;
    tc.text.capture_rendered_text = true;
    u16 left = 0, right = 0;
    tc.text.screenleft = &left;
    tc.text.screenright = &right;
    Book book(tc.ctx);
    parsedata_t p = MakeParseData(tc, book);
    p.pen.y = tc.text.margin.top + tc.text.GetHeight();
    std::string html = "<html><head><style>.sample{text-align:";
    html += alignment == 1 ? "center" : "right";
    html += "}</style></head><body><p class='sample'>A<br/>BBB<br/>CC</p>"
            "<p class='sample'>";
    for (int i = 0; i < 500; ++i) html += "word ";
    html += "end</p></body></html>";
    const auto sheets = epub_stylesheet_utils::ExtractHeadStylesheets(html);
    for (const auto &sheet : sheets)
      epub_css_class_map::ParseCssIntoClassMap(sheet.css.data(), sheet.css.size(),
                                              &p.css_class_map);
    ExpectTrue("embedded CSS XML parses",
               xml_parse_utils::ParseXmlString(html, MakeXmlOpts(&p)).ok);
    ExpectTrue("embedded CSS text crosses pages", book.GetPageCount() > 1);
    for (int page = 0; page < book.GetPageCount(); ++page) {
      tc.text.rendered_glyphs.clear();
      book.GetPage(page)->Draw(&tc.text);
      ExpectAlignedGlyphLines(tc.text, alignment);
    }
  }
}

void TestXmlAlignedLines() {
  for (unsigned char orientation = 0; orientation < 3; orientation++) {
    for (int alignment : {1, 2}) {
      for (bool coalesced : {false, true}) {
        TestCtx tc;
        tc.ctx.orientation = &orientation;
        tc.paragraph_spacing = 0;
        tc.text.display.width = 240;
        tc.text.landscape = orientation_utils::IsLandscape(orientation);
        tc.text.capture_rendered_text = true;
        u16 left = 0, right = 0;
        tc.text.screenleft = &left;
        tc.text.screenright = &right;
        Book book(tc.ctx);
        parsedata_t p = MakeParseData(tc, book);
        p.pen.y = tc.text.margin.top + tc.text.GetHeight();
        p.coalesce_text_segments = coalesced;
        std::string html = "<html><body><p style=\"text-align:";
        html += alignment == 1 ? "center" : "right";
        html += "\">A<br/><br/>BBB<br/>CC</p><p style=\"text-align:";
        html += alignment == 1 ? "center" : "right";
        html += "\">";
        for (int i = 0; i < 500; i++) html += "word ";
        html += "end</p></body></html>";
        ExpectTrue("aligned XML parses",
                   xml_parse_utils::ParseXmlString(html, MakeXmlOpts(&p)).ok);
        ExpectTrue("aligned paragraph crosses pages", book.GetPageCount() > 1);
        for (int page = 0; page < book.GetPageCount(); page++) {
          tc.text.rendered_glyphs.clear();
          book.GetPage(page)->Draw(&tc.text);
          ExpectAlignedGlyphLines(tc.text, alignment);
          if (page == 0) {
            ExpectIntEq("alignment preserves explicit blank lines",
                        tc.text.rendered_glyphs[1].y -
                            tc.text.rendered_glyphs[0].y,
                        2 * (tc.text.GetHeight() + tc.text.linespacing));
          }
        }
      }
    }
  }
}

} // namespace

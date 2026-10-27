#pragma once

// These tests exercise Page::Draw, with the Text spy counting real measurement
// requests as well as ordinary glyph drawing. No cache internals are exposed.
void TestPageAlignmentRedrawMeasurements() {
  for (unsigned char orientation = 0; orientation < 3; ++orientation) {
    Text text;
    text.capture_rendered_text = true;
    text.landscape = orientation == 2;
    text.display.width = 240;
    std::vector<u16> left(400 * 400), right(400 * 400);
    text.screenleft = left.data();
    text.screenright = right.data();
    BookContext ctx;
    ctx.text = &text;
    ctx.orientation = &orientation;
    Book book(ctx);
    Page page(&book);
    const u32 tokens[] = {
        TEXT_PARAGRAPH_CENTER, TEXT_LINE_START_X, 48,
        'A', TEXT_BOLD_ON, 'B', TEXT_ITALIC_ON, 'C',
        TEXT_MONO_ON, 'D', TEXT_BOLD_OFF, TEXT_ITALIC_OFF, 'E',
        TEXT_MONO_OFF, '\n', TEXT_PARAGRAPH_RIGHT, TEXT_FONT_SIZE, 28,
        TEXT_LINK_START, 1, 'X', 'Y', TEXT_LINK_END,
        TEXT_FONT_SIZE, 14, '\n', TEXT_SCREEN_BREAK, TEXT_PARAGRAPH_CENTER,
        TEXT_SUPERSCRIPT_ON, '2', TEXT_SUPERSCRIPT_OFF, 'F', '\n',
        TEXT_SUBSCRIPT_ON, '3', TEXT_SUBSCRIPT_OFF, 'G'};
    page.SetBuffer(tokens, sizeof(tokens) / sizeof(tokens[0]));
    unsigned cold_calls = 0;
    for (int draw = 0; draw < 2; ++draw) {
      text.rendered_glyphs.clear();
      text.advance_calls = 0;
      page.Draw(&text);
      test::ExpectEqU("mixed aligned text retains every glyph", text.rendered_glyphs.size(), 11);
      // The first line is 5 * 8px; both paragraph modes use the base margins,
      // even when a publisher supplied a different TEXT_LINE_START_X.
      const int first_width = orientation == 2 ? 400 : 240;
      const int center_start = orientation == 2 ? 180 : 100;
      for (size_t glyph = 0; glyph < 5; ++glyph)
        test::ExpectEq("centered fragments share one line", text.rendered_glyphs[glyph].x,
                       center_start + (int)glyph * 8);
      test::ExpectEq("right alignment measures active font size",
                     text.rendered_glyphs[5].x, first_width - 12 - 32);
      test::ExpectEq("right aligned second glyph follows first",
                     text.rendered_glyphs[6].x, first_width - 12 - 16);
      test::ExpectEq("superscript keeps smaller glyph size", text.rendered_glyphs[7].width, 5);
      test::ExpectEq("superscript restores following font size", text.rendered_glyphs[8].width, 8);
      test::ExpectEq("subscript keeps smaller glyph size", text.rendered_glyphs[9].width, 5);
      test::ExpectEq("subscript restores following font size", text.rendered_glyphs[10].width, 8);
      test::ExpectTrue("script lines move to second reading screen",
                       text.rendered_glyphs[7].screen ==
                           (orientation == 1 ? text.screenleft : text.screenright));
      if (draw == 0)
        cold_calls = text.advance_calls;
      else
        test::ExpectEqU("redraw skips eleven alignment glyph measurements",
                        cold_calls - text.advance_calls, 11);
    }
  }
}

void TestPageAlignmentCacheInvalidation() {
  Text text;
  text.capture_rendered_text = true;
  text.display.width = 240;
  u16 left = 0, right = 0;
  text.screenleft = &left;
  text.screenright = &right;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  Page page(&book);
  const u32 tokens[] = {TEXT_PARAGRAPH_CENTER, 'A', 'B'};
  page.SetBuffer(tokens, 3);
  page.Draw(&text);

  // Coordinates must be recomputed from the current content box on a warm
  // redraw; a stored start_x would retain the old viewport or margins.
  text.rendered_glyphs.clear();
  text.display.width = 320;
  text.margin.left = 20;
  text.margin.right = 40;
  page.Draw(&text);
  test::ExpectEq("cached width follows changed screen and margins",
                 text.rendered_glyphs[0].x, 142);
  text.SetPixelSize(28);
  text.rendered_glyphs.clear();
  page.Draw(&text);
  test::ExpectEq("font size changes invalidate measured line width",
                 text.rendered_glyphs[0].x, 134);

  text.advance_calls = 0;
  page.Draw(&text);
  const unsigned warm_calls = text.advance_calls;
  text.SetFontFile("replacement.ttf", TEXT_STYLE_REGULAR);
  text.advance_calls = 0;
  page.Draw(&text);
  test::ExpectEqU("font replacement remeasures the line",
                  text.advance_calls - warm_calls, 2);
  book.SetLayoutRevision(book.GetLayoutRevision() + 1);
  text.advance_calls = 0;
  page.Draw(&text);
  test::ExpectEqU("layout revision remeasures the line",
                  text.advance_calls - warm_calls, 2);

  // Reusing the same Page and allocation must not reuse the old text width.
  const u32 replacement[] = {TEXT_PARAGRAPH_CENTER, 'Z'};
  page.SetBuffer(replacement, 2);
  text.rendered_glyphs.clear();
  page.Draw(&text);
  test::ExpectEq("SetBuffer invalidates measurements on reused Page",
                 text.rendered_glyphs[0].x, 142);
  page.FreeBuffer();
  page_buffer_utils::OwnedPageBuffer owned;
  owned.codepoints.assign(tokens, tokens + 3);
  page.AdoptBuffer(&owned);
  text.rendered_glyphs.clear();
  page.Draw(&text);
  test::ExpectEq("AdoptBuffer supports legacy pages without alignment coordinates",
                 text.rendered_glyphs[0].x, 134);

  Page other(&book);
  other.SetBuffer(tokens, 3);
  other.Draw(&text);
  text.advance_calls = 0;
  page.Draw(&text);
  test::ExpectEqU("returning from another page remeasures evicted widths",
                  text.advance_calls - warm_calls, 2);
}

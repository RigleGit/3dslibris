#pragma once

void TestOpaqueInlineCacheBudget() {
  const std::string folder = getenv("TEST_IMAGE_VIEWER_DIR");
  const std::string archive = folder + "/opaque-cache.epub";
  WriteFileBytes(archive, ReadFileBytes(folder + "/images.epub"));
  Text text;
  text.capture_rendered_text = true;
  text.display.width = 240; text.display.height = 400;
  std::vector<u16> screen(400 * 400, 0x07e0);
  text.screenleft = text.screenright = screen.data(); text.SetScreen(screen.data());
  BookContext ctx; ctx.text = &text;
  Book book(ctx); book.SetFolderName(folder); book.SetFileName("opaque-cache.epub");
  book.format = FORMAT_EPUB;
  const u16 image = book.RegisterInlineImage("OEBPS/tiny.png");
  auto draw = [&](int width) {
    std::fill(screen.begin(), screen.end(), 0x07e0);
    text.SetPen(10, 30);
    InlineImageLayoutPlan plan{};
    plan.mode = INLINE_IMAGE_LAYOUT_INLINE;
    plan.draw_width = width; plan.draw_height = 180;
    InlineImageDrawBounds bounds{};
    test::ExpectTrue("opaque image survives within 1 MiB budget",
                     book.DrawInlineImage(&text, image, &plan, 0, 0, &bounds));
    test::ExpectEq("opaque width remains exact", bounds.x1 - bounds.x0, width);
    test::ExpectEq("opaque height remains exact", bounds.y1 - bounds.y0, 180);
    bool exact = true;
    for (int y = 0; y < 400; ++y) for (int x = 0; x < 400; ++x) {
      const bool inside = x >= bounds.x0 && x < bounds.x1 && y >= bounds.y0 && y < bounds.y1;
      exact = exact && screen[y * 400 + x] == (inside ? 0xf800 : 0x07e0);
    }
    test::ExpectTrue("RGB565 blit keeps image and untouched framebuffer exact", exact);
  };
  // These 12 dimension variants occupy 801360 RGB565 bytes; ARGB8888 would
  // exceed 1 MiB and evict the early variants. No cache internals are exposed.
  for (int width = 180; width < 192; ++width) draw(width);
  test::ExpectEq("remove opaque-cache source", remove(archive.c_str()), 0);
  image_memory::Bytes unavailable;
  test::ExpectFalse("no source remains to decode evicted variants",
                    book.ReadInlineImageSource(image, &unavailable));
  text.bgcolor.r = text.bgcolor.g = text.bgcolor.b = 255;
  for (int width = 180; width < 192; ++width) draw(width);
}

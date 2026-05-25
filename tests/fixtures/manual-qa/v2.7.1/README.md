# v2.7.1 Manual QA EPUBs

Packaged files live in `tests/fixtures/books/`.

## v2.7.1-index-position-check.epub

Checks #126.

1. Open the book.
2. Move to Chapter 4.
3. Open the index.
4. Expected: Chapter 4 is selected, not Chapter 1.
5. Repeat from Chapter 5 or Chapter 6.

## v2.7.1-layout-regression-checks.epub

Checks #77, #84, #106, #108, and #128.

1. `#108 Body font-size baseline`: body text should follow the user font size, not shrink to 80%.
2. `#106 Image width unaffected by text size`: the banner inside 200% text should not become huge.
3. `#128 Wide block image placement`: with block display enabled, the top screen should not be blank before the wide image.
4. `#128 Right-aligned author-width image`: the 37.60% image should remain block-sized and align right.
5. `#84 Nested block margins`: each extract wrapper should keep visible spacing even though the child paragraph margins are zero.
6. `Image flow fit diagnostic`: small images should not create a large blank gap before or after themselves.
7. `#77 Block indent across pages`: with EPUB block display, left orientation, and font size 12, indented nested block lines should keep their indent when they cross a screen boundary.
8. `#108 EPUB style settings`: use this chapter to check:
   - Publisher text indent ON/OFF changes the indented sample.
   - Publisher block margins ON/OFF changes the margin sample.
   - Extra paragraph spacing changes the spacing sample.
   - Per-book settings affect this book without changing another EPUB.

Non-EPUB release checks:

- #127 START closes folders and leaves library-opened settings.
- CIA banner music is audible from the installed CIA banner.

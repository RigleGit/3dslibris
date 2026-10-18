# Book text search

Search is available in `BOOK` settings for EPUB, MOBI, TXT, Markdown, FB2,
RTF and ODT after opening and pagination have finished. Select `search`, enter
a word or phrase with the system keyboard, choose `whole words` if wanted,
and select `find`. The first search starts at the beginning of the current page.

In search mode:

- `L` / `R` find the previous / next match, following the reader's handedness
  and swapped-shoulder settings.
- `X` opens the query and whole-word options again.
- `A` / `B` return to reading at the current page.
- While a search is pending, `B` cancels it without changing the page.
- D-Pad page controls can inspect the rest of a match spanning multiple pages.
- `SELECT` opens book settings; `START` returns to the library.

A search wraps once and reports `wrapped`. It does not count all matches before
showing the first one. An unsuccessful or cancelled search leaves the position
unchanged. Results may overlap, but navigating never repeatedly returns the
same active occurrence. Query and whole-word options live only in the current
book session; they are not written to preferences or a search history.

Search ignores case using Unicode case folding, accepts canonically equivalent
composed/decomposed characters, and collapses equivalent whitespace. Accents
remain significant: `cafe` does not match `café`, and `ano` does not match `año`.
Whole words use Unicode letters, marks, numbers and connectors as word characters.
Optional soft hyphens are ignored in both the book and query; real hyphens remain
significant. After paging outside a match, L/R searches from the selected page.
Paging within a multi-page match keeps its anchor and highlight.
The input limit is 128 UTF-16 units; supplementary characters occupy two units.
The system keyboard enables predictive input, required for kanji on Japanese
systems. Available scripts still depend on the console's keyboard.

PDF, CBZ, XPS, OCR and searching across books are excluded. RTL queries are
rejected with an explanation. Reordered RTL runs in mixed content are skipped,
and a completed scan reports that exclusion when it encounters them. Images,
horizontal rules and separate XHTML documents form search boundaries.

After updating, EPUB and MOBI page caches may be rebuilt on first open.
Saved reading progress and preferences are kept.

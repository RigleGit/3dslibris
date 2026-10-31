# Library search

In the library, press `Y`, or open `GENERAL` settings with `SELECT` and choose
`search`. This opens an editor with four actions: search field, query, search
and back. The initial field is **file / folder name**. Select the field to cycle
through **title** and **author**. Select the query to open the system keyboard;
cancelling it keeps the previous query.

Search ignores case and accents using Unicode normalization. All query words
must occur somewhere in the selected field; each word can match part of a word.
For example, `garcia cien` can match `García - Cien años.epub` in Name mode.
The query limit is 128 keyboard characters (up to 512 UTF-8 bytes).

The scope follows the existing one-level library:

- At the virtual root, search visible root entries and the books directly inside
  its visible folders. It uses the active SD library; bundled books are only
  used when the configured books directory is missing.
- Within a folder, search only that folder's direct books.
- Folders appear only in Name mode. No second folder level is visited.

## Results and controls

Five results appear per page, with their source folder below the name/title.
Use D-Pad to select, `L` / `R` to change result pages, and `A` or touch to open.
`Y` / `X` returns to the editor. While scanning/preparing, `B` cancels and returns
to the editor; otherwise `B` closes search. Results are capped at 256 and the
screen asks for a narrower query when the result or memory limit is reached.

Closing search restores the original location, selection and browser page.
A disappeared location falls back to the virtual root. Opening a result uses
the regular book/folder flow. After reading, the library shows that book's
location; root books remain at the virtual root. Failed opens retain the query
and results with a message. Normal library `B` / `START` navigation is unchanged.

## Name versus metadata

**Name** compares filenames and folder names directly. It never prepares
metadata, indexes books or loads covers. The directory cursor processes at most
16 entries or a cooperative 4 ms per update.

**Title** and **Author** prepare both genuine fields together for EPUB, PDF,
XPS/OXPS, MOBI, FB2 and CBZ. Other formats are excluded and reported in the
incomplete-search count. A missing field stays empty; the filename used as a
visual fallback in the library does not become searchable metadata.

Preparation reads metadata only: no pagination, text decompression, page
rasterization or image decoding. It uses an individual cache keyed by path,
file size and modification time. Old caches remain usable for library display;
a metadata search verifies them once before trusting title/author. Successful
empty metadata is also cached. Switching Title to Author reuses the same cache.
No page/cover cache clearing is needed.

At most one extraction starts per update. **Cancellation and HOME/sleep are
handled between books**: one synchronous extraction may exceed the directory
budget. Returning from HOME/sleep retains field and query, then restarts the
scan using cached preparations. Correctly prepared records are saved immediately;
if a save fails, their fields still participate in the current search and a
warning explains that preparation may repeat later.

Errors, excluded formats and oversized/corrupt metadata produce an incomplete
search warning. Each searchable field is limited to 4096 UTF-8 bytes; XML/MOBI
metadata reads and MuPDF metadata allocations are bounded to 4 MiB. An exceeded
limit is an error, rather than a silently truncated searchable value. Ancillary
bibliographic display text, such as a long description, is bounded separately
and marked with `...`; it does not exclude an otherwise searchable book.

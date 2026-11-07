# Preload book data

Open `GENERAL` settings with `SELECT`, then choose **preload book data**.
The action is available in both gallery and list view. It prepares metadata and
cover thumbnails across the whole library, even when opened from a folder.
It starts after showing the progress screen, without a confirmation dialog.

The scope is the virtual root and the books directly inside its visible folders.
The configured SD library is used when its directory exists, even when empty;
bundled books are only a fallback for a missing directory. A second folder
level is never visited.
Unlike search results, the operation has no 256-book limit.

## Progress and cancellation

The screen shows the current file and stage, books examined, directory errors,
and separate metadata and cover counts:

- **cached:** existing valid data reused;
- **saved:** newly prepared data written successfully;
- **absent:** no cover found;
- **unsupported:** no extractor for that stage;
- **errors:** failed reads, extraction, memory admission or writes.

Press `B` or touch **cancel** to stop between operations. One cover extraction
can take longer than a frame; cancellation and HOME are handled before starting
the next operation. Completion and cancellation show a summary; use `B`, `A` or
touch **back** to return to preferences. A write failure stops the operation and
keeps everything already saved. Resolve the SD problem and run the action again.
Other individual book errors do not stop the remaining books.

When work finishes, a large **PRELOAD COMPLETE** banner replaces the active
file/stage view. Errors and cancellation have their own completion messages.
The **back to settings** button and `A / B: return to settings` instruction
confirm that preparation has stopped.

After HOME or sleep, the operation restarts its traversal and counters while
reusing saved data. Returning to the library preserves its location and selected
book, even when new titles change the sort order. If the folder disappeared,
the library returns to its root.

## Prepared data and storage

Metadata preparation supports **EPUB, PDF, XPS/OXPS, MOBI, FB2 and CBZ**. Title
and author are prepared together using the same cache as library search. Empty
fields are valid; old records are verified once. Other formats are counted as
unsupported for metadata, but may still have an external cover.

Cover preparation reuses valid thumbnails first, then tries the existing
external-cover rules and the EPUB/MOBI/FB2/PDF/XPS/CBZ extractors. Folder covers
are included when an associated image exists. PDF/XPS uses only a cover
thumbnail, with the existing memory and page-complexity checks. No reading
pages, pagination or normal reader session are prepared.

**Starting the first manual preload enables persistent cover retention.** All
cached thumbnails stay on the SD until **clear cache** is used; the normal
512-file / 16 MiB automatic pruning is disabled, including after restarting the
app. A full 85×115 RGB565 thumbnail uses about 19 KiB before filesystem overhead
(about 19 MiB for 1,000 thumbnails), plus metadata and cache diagnostics. Large
libraries therefore require additional free SD space. Original book images are
not copied into this cache.

Retention uses a marker in the existing cover-cache directory. If it cannot be
saved, preload does not start. Clearing the cache removes the marker and restores
the automatic limit. No manual cache deletion or page-cache migration is needed.
The operation keeps only one temporary book and thumbnail in RAM, not the full
cover collection. Running preload again reuses valid saved data; corrupt or
outdated cache entries are regenerated.

## Physical validation

Host tests and builds do not replace testing on New Nintendo 2DS XL and Nintendo
3DS XL. Check both CIA and 3DSX:

1. Start from a root and from a folder, in list and gallery views. Include more
   than 256 books / 512 covers and confirm there is no second-level traversal.
2. Run twice; check cached/saved counts, external folder covers, unsupported
   metadata formats, corrupt books and complex PDF covers.
3. Cancel during preparation; retry; use HOME and sleep; verify saved work is
   reused and controls remain responsive between operations.
4. Return to preferences and library; verify selection, location, new labels
   and thumbnails. Restart and verify retained covers. Clear cache and confirm
   the usual retention limit is restored.
5. Test an unwritable/full SD and low-memory conditions; verify the failure
   summary and that previously saved data remains usable.

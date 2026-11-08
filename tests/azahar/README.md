# Azahar / MCP font-size regression (#172)

Opt-in integration test for the real ARM application through `azahar-mcp`'s
stdio protocol. It is intentionally outside `make test-host`: it opens and
closes an emulator window and requires Azahar plus the local MCP project.

## Run

First build the usual debug application (`make debug-3dsx` in the Docker build
environment). Its objects are reused to prepare two separate probe binaries:

```sh
bash tests/azahar/build_font_probe.sh

uv run --project /path/to/azahar-mcp python tests/azahar/font_size_regression.py \
  --mcp-repo /path/to/azahar-mcp \
  --rom .private/azahar-font-probe/3dslibris-font-current.3dsx \
  --reference-rom .private/azahar-font-probe/3dslibris-font-baseline.3dsx \
  --output .private/azahar-font-run-01
```

The output directory must be new for each invocation. `AZAHAR_PATH` can select
the emulator, as documented by `azahar-mcp`. Use `--orientation portrait` or
`--orientation landscape` and `--sizes 14 18` for a shorter current-build run.
The reference always exercises 14 and 18 px. The default current matrix is
8, 12, 14, 16, 18 and 20 px in both orientations, using the New 3DS profile.

`IMAGE_TAG` overrides the build image (`3dslibris-cia:release` by default).
`FONT_BASELINE_REF` overrides the original font-code revision (`3db90f51`).
The baseline combines that font implementation with the current application
and cache code, isolating the font behavior without migrating old preferences.
It is a negative control, not a distributable historical release.

## What is checked

- A real MCP handshake and a matching ELF/mailbox, live frame heartbeat,
  acknowledged buttons, and a touch that changes the font-size override.
- The selected per-book size actually reaches `Text` and the FreeType
  Armenian fallback. Member offsets come from the matching ARM ELF's DWARF;
  no runtime addresses are hardcoded and no application state is patched.
- A generated EPUB is resident and contains the intended fixture text.
- The renderer's actual clipping diagnostics show **zero clipped book
  glyphs**. All diagnostic records must agree with the live event counter,
  so disabled, exhausted or missing logs cannot yield a false pass.
- Browser and browser fallback remain 12 px after leaving the reader.
- Warm reopen in a **new emulator process**, with a copy of the first test
  SDMC, preserves the override, has a resident backend, produces no clipped
  book glyphs, and leaves the persistent cache files unchanged. START/A alone
  would reuse the Book object and is not a disk-cache test.
- The pre-fix control must fail with actual clipped book glyphs. A failed
  launch, timeout, screenshot failure or missing symbols do not satisfy it.

`TXT clipR` also reports UI ink beyond the *layout* margin with `clip=0`.
Those style-4 records are retained as evidence but are not book-text failures.
The counter is a completeness check; its total is not itself a failure count.

The regular debug build compiles out TRACE-level `TXT clipR` messages. The
probe builder recompiles the renderer and log filter with TRACE enabled in
separate build directories. It never changes source files, main debug objects,
or the normal/debug distribution binaries. The test writes only existing
diagnostic level/category/budget controls through MCP memory tools.

## Evidence and isolation

Each run produces `summary.json`, per-run `result.json`, the MCP call/response
transcript, emulator/app logs, screenshots of both screens, and server stderr.
Current builds must pass; specified controls must fail for the clipping defect.
The process exit code is nonzero for an application failure or an inconclusive
MCP run. Read `error` separately from per-size `failures` when diagnosing it.
Missing/abnormal emulator shutdown also invalidates an otherwise passing run.

The evidence-validation guards can be checked without opening Azahar:

```sh
PYTHONDONTWRITEBYTECODE=1 uv run --project /path/to/azahar-mcp \
  python -m unittest discover -s tests/azahar -p test_evidence.py
```

The fixture uses a fresh copied SDMC containing only bundled fonts and one
generated EPUB; the user's library, preferences and cache are not used.
The emulator is stopped and logs are archived in a `finally` block. On macOS
the test requires an Azahar-owned window before requesting a screenshot, to
avoid the MCP capture tool's whole-desktop fallback.

This validates emulator automation and the exercised font/layout path. It
does not prove physical 3DS memory/timing, HOME/sleep behavior, every Unicode
script/alignment, CIA installation, or the complete fixed-layout GPU path.

# Reader regression on the debug 3dsx (search, EPUB/MOBI, cache)

`changes_regression.py` drives the real **debug** application
(`3dslibris-debug.3dsx` plus its matching `.elf`) through azahar-mcp. Like the
font test it is opt-in and manual: it is not part of `make test-host`, it is
not run by CI, and it needs Azahar, an azahar-mcp checkout with its `.venv`
(`elftools`, `mcp`, `Pillow`) and a debug build from the Docker workflow
(`make debug-3dsx`).

```sh
MCP_REPO=/path/to/azahar-mcp
"$MCP_REPO/.venv/bin/python" tests/azahar/changes_regression.py \
  --mcp-repo "$MCP_REPO" --rom 3dslibris-debug.3dsx \
  --role current --output .private/azahar-changes-run-01
```

The output directory must be new. `--scenarios` selects any of `epub`, `warm`,
`mobi`, `long` (default: all, about 10 minutes); `--cases` limits the search
matrix inside `epub`. Fixtures are generated by `changes_fixtures.py` into a
copied SDMC, never the user's.

## Scenarios and oracles

- `epub`: a four-chapter EPUB (English with curly quotes, Cyrillic, Greek,
  Arabic RTL, accents, an inline image, shared and reordered stylesheets).
  Checks TOC chapters, per-page token content, RTL/shaped Arabic tokens, then
  the **search matrix**: ASCII, Cyrillic case folding, Greek, `café` vs `cafe`,
  whole words, a phrase across wrapped lines, text after an image, an absent
  term, an unsupported RTL query, next/previous and wrap-around. Every match
  is confirmed by an independent Python normalizer reading the page tokens
  from emulator memory (`changes_lib.matches_at`), not by the search engine.
- `warm`: new emulator process with a copy of the cold SDMC; every page must
  hash equal to the cold parse (persistent cache round trip).
- `mobi`: generated MOBI (`tests/fixtures/generate_mobi_reading.py`); TOC
  titles and order, chapter pages, end-of-book text, hidden script content.
  A known pre-existing mismatch exists: chapter 1 maps to a later page than
  its heading (identical on commit `cd78449f`), so one check fails today.
- `long`: ~230-page EPUB whose cache exceeds the 64 KB read window; cold
  parse, restart, then forward and backward paging must reproduce the same
  page hashes and monotone paragraph ids.

## Search seam (debug builds only)

The system keyboard applet opens a host dialog that an emulator test cannot
type into. In `DSLIBRIS_DEBUG` builds, `ReaderController::EditSearchText`
reads the query from `sdmc:/3ds/3dslibris/search_query.txt` when that file
exists; the test writes it through `azahar_manage_sdmc`. Everything after the
text entry is the real search path. Release builds do not contain the seam.

## Comparing against another build

Run the same command with `--role baseline` and a debug 3dsx built from the
previous commit (with the same seam applied), then:

```sh
"$MCP_REPO/.venv/bin/python" tests/azahar/compare_runs.py RUN_BASELINE RUN_CURRENT
```

It compares page count, chapters, page hashes, cache files, MOBI results,
search positions/status/tokens and screenshot pixels, and prints `scan_ms`
for both. Azahar timings are indicative only; they say nothing about physical
Old/New 3DS speed, memory or SD latency.

# Reader input regression (#177, #178, #181, #182)

`reader_input_regression.py` checks reader and Settings input on the debug
3dsx. Same requirements as `changes_regression.py`; it is manual and opt-in.

```sh
MCP_REPO=/path/to/azahar-mcp
"$MCP_REPO/.venv/bin/python" tests/azahar/reader_input_regression.py \
  --mcp-repo "$MCP_REPO" --rom 3dslibris-debug.3dsx \
  --output .private/azahar-reader-input-run-01
```

`--scenarios` selects any of `178`, `182`, `177`, `181` (default: all).
The book is `tests/fixtures/epub-render-test/`, zipped into a copied SDMC.
State comes from live memory (`Book` position and bookmarks, link focus,
Settings page and selection) plus screenshots:

- `178`: landscape D-pad UP/DOWN only jump between bookmarks; held LEFT/RIGHT
  repeat page turns; Circle Pad with `circlePadPageTurn="0"` turns one page.
- `182`: held D-pad keeps moving link/image focus, with Y still held and
  after releasing it, across pages, in landscape and portrait.
- `177`: landscape HUD shows `N/M*` on bookmarked pages; portrait keeps `< N* >`.
- `181`: Settings paging in both handedness modes with D-pad and L/R; going
  back lands on the previous page's last slot; UP/DOWN still change values.

Overlapping holds are written straight to the input mailbox because
`azahar_press_buttons` blocks until each command ends. Azahar input timing is
not hardware-accurate.

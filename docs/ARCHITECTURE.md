# Architecture

High-level architecture of 3dslibris as of v2.0.3. This document describes the current structure, module responsibilities, and known design trade-offs.

## Module map

```
source/
├── main.cpp                    # Entry point: 3DS bootstrap, service init, App lifecycle
├── app/                        # Application orchestration + lifecycle controllers
│   ├── app.cpp                 # App wiring, shared state accessors, navigation helpers
│   ├── startup_controller.cpp  # Boot sequence and runtime asset checks
│   ├── main_loop_controller.cpp# Main loop dispatch (mode routing + frame present)
│   ├── status_controller.cpp   # Status bar rendering/update cadence
│   └── status_layout_utils.cpp # Status bar layout
├── library/                    # Library/browser screen
│   ├── app_browser.cpp         # Book grid, cover cache, metadata jobs
│   ├── library_controller.cpp  # Library scan + browser flow delegation target
│   └── browser_*.h             # Cover cache, job queue, warmup utilities
├── reader/                     # Book reader screen
│   ├── app_book.cpp            # Open/reopen flow, input routing, deferred relayout
│   ├── reader_controller.cpp   # Reader flow delegation target
│   └── deferred_relayout_utils.h
├── book/                       # Book domain model + page management
│   ├── book.cpp                # Metadata, chapters, bookmarks, page vector
│   ├── book_fixed_layout.cpp   # PDF/CBZ viewport interaction
│   ├── book_inline_image.cpp   # Inline image layout for reflowable formats
│   ├── heading_layout.cpp      # Heading/chapter title layout
│   ├── inline_image_layout.cpp # Image placement algorithm
│   ├── layout_reflow.cpp       # Layout recalculation on settings change
│   ├── page.cpp                # Page buffer (text + layout tokens)
│   ├── reflow_worker.cpp       # Background reflow for New 3DS
│   ├── page_buffer_utils.h     # Page buffer serialization
│   └── reflow_cache_save_utils.h
├── formats/                    # Format-specific parsers
│   ├── common/                 # Shared format utilities
│   │   ├── book_io.cpp         # Thin format dispatcher
│   │   ├── buffered_status_log.cpp
│   │   ├── epub_image_utils.cpp
│   │   ├── file_read_utils.cpp
│   │   ├── page_cache_utils.cpp
│   │   ├── plain_parser.cpp   # TXT/RTF parse flow + plain heading heuristics/callbacks
│   │   ├── plain_text_stream.cpp # Incremental plain text pagination state machine
│   │   ├── text_helpers.cpp    # Text normalization (UTF repair, RTF decode, etc.)
│   │   ├── xml_book_parser.cpp # XML/Fb2 parse route extracted from Book::Parse
│   │   ├── xml_parse_utils.cpp
│   │   ├── pdf_view_utils.cpp  # Shared MuPDF viewport/navigation
│   │   ├── fixed_layout_viewport_utils.h
│   │   └── rtf_control_word_utils.h
│   ├── txt/                    # Plain text loader
│   │   └── txt_loader.cpp     # ReadAndNormalize() with CP1252 repair
│   ├── rtf/                    # Rich Text Format loader
│   │   └── rtf_loader.cpp     # ReadAndDecode() with RTF→UTF-8
│   ├── epub/                   # EPUB2/EPUB3 parser
│   │   └── epub.cpp            # Full EPUB parsing + page cache
│   ├── fb2/                    # FictionBook 2 parser
│   │   └── fb2.cpp
│   ├── mobi/                   # MOBI/KF8 parser
│   │   ├── mobi.cpp            # Cover extraction
│   │   ├── mobi_cover_meta_cache.cpp
│   │   ├── mobi_heading_markers.cpp
│   │   ├── mobi_page_cache.cpp  # Persistent page cache serialization
│   │   ├── mobi_parser_core.cpp # Source load/header parse/text record merge
│   │   ├── mobi_deferred_runtime.cpp # Deferred parse state machine runtime
│   │   ├── mobi_markup_extract.cpp # MOBI markup -> text extraction pipeline
│   │   ├── mobi_book_hooks.cpp  # Book-facing MOBI hook/callback wiring
│   │   ├── mobi_parser.cpp      # MOBI parse orchestration (open + deferred finalize)
│   │   ├── mobi_markup_tag.cpp
│   │   ├── mobi_position_map.cpp
│   │   ├── mobi_record_decode.cpp
│   │   ├── mobi_record_scan.cpp
│   │   ├── mobi_structured_toc_parser.cpp # INDX/TAGX/CNCX structured TOC parser
│   │   ├── mobi_toc_finalize.cpp # Structured/heuristic TOC finalization
│   │   ├── mobi_toc_apply.cpp    # Structured TOC mapping/apply (html_pos -> page)
│   │   ├── mobi_toc_prepare.cpp  # Structured TOC prepare/deferred-load wrapper
│   │   ├── mobi_toc_resolver.cpp # TOC resolver + inline filepos/deferred fallback
│   │   ├── mobi_text_decode.cpp  # Encoding decode + embedded title extraction
│   │   ├── mobi_text_cleanup.cpp
│   │   └── mobi_*.h            # Cover utils, deferred finalize
│   ├── pdf/                    # PDF entry point
│   │   └── pdf.cpp
│   ├── cbz/                    # CBZ entry point + rendering
│   │   ├── cbz.cpp
│   │   ├── cbz_archive.cpp
│   │   ├── cbz_decode.cpp
│   │   ├── cbz_document.cpp
│   │   ├── cbz_view.cpp
│   │   └── cbz_worker.cpp
│   └── mupdf/                  # MuPDF integration (PDF/CBZ/XPS)
│       ├── mupdf_common.cpp/h
│       ├── mupdf_document.cpp
│       ├── mupdf_render.cpp/h
│       ├── mupdf_view.cpp/h
│       ├── mupdf_worker.cpp/h
│       └── mupdf_state.h
├── ui/                         # UI primitives
│   ├── text.cpp                # FreeType text renderer, framebuffers
│   ├── button.cpp              # Button rendering + hit-testing
│   ├── ui_button_skin.cpp      # Procedural button skins + icon loading
│   ├── touch_utils.cpp         # Touch gesture interpretation
│   ├── browser_nav.cpp         # Browser navigation helpers
│   ├── framebuffer_blit_utils.h
│   ├── glyph_cache_lru.h
│   └── text_buffer_utils.h
├── menus/                      # Overlay menus
│   ├── menu.cpp
│   ├── bookmark_menu.cpp
│   ├── chapter_menu.cpp
│   └── paged_list_menu.cpp
├── settings/                   # Settings/prefs management
│   ├── prefs.cpp               # Persistent preferences (XML)
│   ├── font.cpp                # Font loading + metrics
│   └── app_prefs.cpp           # Settings screen logic
├── shared/                     # Cross-cutting utilities (genuinely shared)
│   ├── app_flow_utils.cpp/h    # Format detection, path conversion
│   ├── text_layout_utils.cpp/h # Text layout helpers
│   ├── text_unicode_utils.cpp/h# Unicode/UTF-8 utilities
│   └── utf8_utils.cpp/h        # UTF-8 encoding/decoding
└── core/
    └── parse.cpp               # Parser dispatch entry point

include/                        # Public headers (mirrors source/ structure)
└── string_utils.h             # Inline string utilities (StartsWithNoCase, etc.)

third_party/                    # External dependencies
├── expat/                      # XML parser (moved from source/expat)
├── stb/                        # stb_image header
├── utf8proc/                   # UTF-8 normalization
├── libunibreak/                # Line breaking
└── mupdf/                      # MuPDF (PDF/CBZ/XPS rendering)
```

## Data flow

```
App::Run()
  ├─ StartupController::RunBootSequence()
  └─ MainLoopController::RunMainLoop() (input → mode dispatch → draw → frame swap)
       │
       ├─ Browser mode
       │    ├─ FindBooks() → scan bookdir → create Book objects
       │    ├─ Job queue → extract covers, index metadata, resolve TOC
       │    └─ Cover cache → load/evict RGB565 thumbnails
       │
       ├─ Opening mode
       │    └─ OpenBook() → parse → paginate → switch to Book mode
       │
       ├─ Book mode
       │    ├─ Page navigation (A/B/L/R, touch, D-pad)
       │    ├─ Deferred relayout (MOBI async reflow on New 3DS)
       │    └─ Viewport interaction (PDF/CBZ zoom + pan)
       │
       └─ Settings mode
            └─ Font size, spacing, orientation, color mode
```

## Format pipeline

```
Book::Open()
  └─ parse.cpp dispatches by extension
       │
       ├─ EPUB  → epub.cpp (minizip + expat XML → pages)
       ├─ FB2   → fb2.cpp (expat XML → pages)
       ├─ MOBI  → mobi_parser.cpp + mobi/*.cpp (PDB records → pages)
       ├─ TXT   → txt_loader.cpp (raw text → pages)
       ├─ RTF   → rtf_loader.cpp (RTF control words → pages)
       ├─ ODT   → book_io.cpp (minizip + expat → pages)
       ├─ PDF   → mupdf/*.cpp (MuPDF → display lists → bitmaps)
       ├─ CBZ   → cbz/*.cpp + mupdf/*.cpp (zip → images → bitmaps)
       └─ XPS   → mupdf/*.cpp (MuPDF → display lists → bitmaps)
```

Reflowable formats (EPUB, FB2, MOBI, TXT, RTF, ODT) produce `Page` objects with text buffers and layout tokens. Fixed-layout formats (PDF, CBZ, XPS) produce bitmaps via MuPDF.

## Known design trade-offs

### 1. App monolith reduction in progress
`App` no longer owns the full runtime loop implementation directly. `StartupController`, `MainLoopController`, `LibraryController`, `ReaderController`, `SettingsController`, and `StatusController` now host major flows, while `App` delegates via explicit methods.

**Impact:** Coupling was reduced (notably by removing controller `friend` access and introducing explicit `App` APIs), but `App` is still a broad facade and remains the central mutable state hub.

**Current direction:** Continue migrating mode-specific mutable state (`job_queue`, browser runtime details, reader deferred/opening state) behind controller-owned interfaces so `App` can converge toward orchestration-only responsibilities.

### 2. Book mixes model, parsing, and rendering
`Book` contains metadata (good), but also parsing methods (`Open()`, `Parse()`, `Index()`) and rendering methods (`DrawCurrentView()`, `DrawCurrentMuPdfView()`). It also holds an `App*` pointer creating a circular dependency.

**Impact:** Cannot reuse Book outside this project. Format-specific state (MuPdfState, CbzState) lives in the domain model.

**Future direction:** Separate Book (pure model) from BookParser (format-specific) and BookRenderer (format-specific).

### 3. book_io.cpp reduced (66 lines, down from 5369)
TXT/RTF parse flow, plain heading heuristics/callbacks, text normalization helpers, MOBI page cache, MOBI parser core helpers (source/header/merge), MOBI deferred runtime state machine, MOBI markup extraction pipeline, MOBI text decode/title extraction, MOBI structured TOC INDX/TAGX/CNCX parsing, MOBI TOC finalization, MOBI TOC prepare/deferred-load wrappers, MOBI TOC resolver (inline/deferred), MOBI parse orchestration, MOBI hook/callback wiring, and XML parse flow were extracted to separate modules. Remaining content: format dispatch only.

**Impact:** Format/parser changes no longer touch `book_io.cpp` in practice, except when adding/removing dispatch routes.

**Future direction:** Keep `book_io.cpp` as a thin dispatch unit. Optional next step: a small dispatch table helper to simplify extension registration.

### 4. Formats extracted from book_io.cpp
The following modules were extracted to improve testability and reduce monolith size:

| Module | From | Public API |
|--------|------|------------|
| `txt_loader` | book_io.cpp | `ReadAndNormalize()` — reads TXT, repairs CP1252 mojibake, normalizes newlines |
| `rtf_loader` | book_io.cpp | `ReadAndDecode()` — reads RTF, decodes to UTF-8 via `text_helpers` |
| `text_helpers` | book_io.cpp | `NormalizeNewlines`, `NormalizeTextUtf8`, `DecodeRtfToUtf8`, `LooksLikeValidUtf8Bytes` |
| `plain_text_stream` | book_io.cpp | `InitState()`, `ContinueState()` — incremental pagination state machine |
| `plain_parser` | book_io.cpp | `ParseBuffer()`, `ParseTxtFile()`, `ParseRtfFile()`, heading heuristics/callback wiring |
| `xml_book_parser` | book_io.cpp | `ParseXmlBookFile()` — XML/Fb2 parse path and perf/error reporting |
| `mobi_book_hooks` | book_io.cpp | `Make()` — MOBI callbacks/hook wiring for parser runtime |
| `mobi_page_cache` | book_io.cpp | `TryLoad()`, `Save()` — persistent page cache for MOBI |
| `mobi_parser_core` | book_io.cpp | `LoadMobiSource()`, `ParseMobiHeader()`, `BuildMobiMergedText()` |
| `mobi_deferred_runtime` | book_io.cpp | `Continue()`, `Finalize()`, deferred state map lifecycle |
| `mobi_markup_extract` | book_io.cpp | `ExtractToText()` — markup parsing, heading hints, inline image contexts |
| `mobi_text_decode` | book_io.cpp | `DecodeBytesToUtf8()`, `ApplyEmbeddedTitle()` |
| `mobi_parser` | book_io.cpp | `ParseFile()`, `ContinueDeferredParse()` — MOBI open/deferred orchestration |
| `mobi_structured_toc_parser` | book_io.cpp | `ParseStructuredToc()` — INDX/TAGX/CNCX parser with callback-based decoding/filtering |
| `mobi_toc_finalize` | book_io.cpp | `BuildChaptersFromHints()`, `FinalizePreparedToc()` — TOC finalization + confidence |
| `mobi_toc_apply` | book_io.cpp | `HtmlPosToPage()`, `BuildChaptersFromStructuredToc()` |
| `mobi_toc_prepare` | book_io.cpp | `Prepare()`, `LoadDeferred()` — structured TOC prepare/deferred load wrappers |
| `mobi_toc_resolver` | book_io.cpp | `ParseInlineFileposToc()`, `PrepareStructuredToc()`, `LoadDeferredStructuredToc()` |
| `StartsWithNoCase` | epub_image_utils.cpp | Generic string utility → `string_utils.h` |

All extracted modules have corresponding test suites using the project's `test_build.sh` helper.

## Build system

- **Target:** `3dslibris.3dsx` (homebrew) and `3dslibris.cia` (installable)
- **Toolchain:** devkitARM (ARM11 + ARM9 for New 3DS syscore)
- **Docker:** `docker/Dockerfile.cia` for reproducible builds
- **CI:** GitHub Actions (`.github/workflows/ci.yml`)
- **Tests:** Host-compiled C++ tests in `tests/` run via shell scripts

## Path constants

All SD card paths are centralized in `include/path_utils.h` under the `paths::` namespace. If the directory layout changes, update constants there only.

## Architectural review (v2.0.3)

Critical design issues identified during architectural audit, ordered by severity.

### Critical: App ↔ Book circular dependency

`Book` holds an `App*` pointer and includes `app/app.h`. `App` holds `std::vector<Book*>` and includes `book/book.h`. A global `App *app` variable in `main.cpp` is accessed directly from 23+ files.

**Impact:** Book cannot be tested or reused outside this project. Any refactor of one requires modifying the other. Format parsers reach into App state through Book.

**Evidence:** `include/book/book.h:122` (`App *app`), `source/main.cpp:32` (`App *app` global), 23 files include `app/app.h`, 21 files include `book/book.h`.

**Future direction:** Remove `App*` from Book. Pass required state (`Text*`, `Prefs*`, margins, orientation) as explicit parameters via a `RenderParams` or `BookContext` struct. Eliminate the global `app` variable.

### Critical: Format layer coupled to App/UI

Format parsers still have App/UI coupling in some modules (`epub.cpp`, `mobi.cpp`) where `app/app.h` is included and parser code reaches UI/application state.

**Impact:** Parsers depend on the UI layer. Changing UI can break parsing. Cannot test parsers without App stubs.

**Evidence:** `source/formats/mobi/mobi.cpp:12`, `source/formats/epub/epub.cpp:33` include `app/app.h`. `book_io.cpp` no longer does.

**Future direction:** Define pure interfaces (`IStatusLogger`, `ParseContext`) that parsers receive instead of `App*`. App implements these interfaces.

### Critical: book_io.cpp remains a monolith (resolved)

Recent extractions (txt_loader, rtf_loader, text_helpers, plain_text_stream, plain_parser, xml_book_parser, mobi_page_cache, mobi_parser_core, mobi_deferred_runtime, mobi_markup_extract, mobi_text_decode, mobi_parser, mobi_book_hooks, mobi_structured_toc_parser, mobi_toc_finalize, mobi_toc_prepare, mobi_toc_resolver) removed parser logic from `book_io.cpp`.

**Impact:** `book_io.cpp` is now a low-risk dispatcher. Parser work happens in dedicated modules.

**Future direction:** Keep dispatcher slim and evolve parser modules independently. Optional: move ODT route to dedicated dispatcher table.

### High: epub.cpp remains a large critical parser (1328 lines)

Contains EPUB parsing, page cache management, inline image handling, TOC extraction, and fallback logic in one compilation unit.

**Impact:** No dedicated tests. Any change still risks the most-used format.

**Future direction:** Extract `epub_cache.cpp`, `epub_toc.cpp`, `epub_manifest.cpp`.

### High: Zero tests on critical components

Core runtime units still lack broad integration coverage (`source/app/app.cpp` now 686 lines, `source/reader/app_book.cpp` 903 lines, `source/formats/epub/epub.cpp` 1328 lines). Existing tests mostly cover pure utilities.

**Impact:** Every refactor is a leap of faith. No safety net for the most complex code.

**Future direction:** Adopt a header-only test framework (Catch2/doctest). Add integration tests that load minimal books. Centralize test macros.

### High: UI and business logic mixed across layers

`Text` (1349 lines) mixes FreeType rendering, gradient generation, font path resolution, and UI label rendering. `App` is smaller than before but still mixes orchestration with shared runtime state and compatibility wrapper methods.

**Impact:** Changing the renderer requires recompiling App. Changing UI requires understanding business logic.

**Future direction:** Separate `TextRenderer` (FreeType pure) from `FontManager` (config, paths, metrics). Extract `UIManager` from App.

### Medium: Inconsistent third-party dependency management

Some deps are vendored as source (expat, utf8proc, libunibreak), some as precompiled binaries (libbz2.a, libexpat.a in lib/), MuPDF has a custom build script. No unified version tracking.

**Future direction:** Unify strategy — all as source compiled by Makefile, or all as git submodules. Document versions in `DEPENDENCIES.md`.

### Medium: Global `App *app` variable

`source/main.cpp:32` exposes `App *app` globally. Any file including `main.h` can access full application state.

**Impact:** Implicit dependencies the compiler cannot verify. Impossible to reason about data flow. Blocks parallel testing.

**Future direction:** Eliminate global. Pass App as explicit parameter. Use context structs for frequently-needed data.

### Medium: No clear extension points for new formats

Adding a format requires modifying `parse.cpp`, `book_io.cpp`, `app_flow_utils.cpp`, `format_t` enum in `book.h`, and `Makefile`. No `FormatParser` interface.

**Future direction:** Define abstract `FormatParser` interface. Register parsers by extension in a map. `parse.cpp` becomes a generic dispatcher.

### Fragile zones

| Zone | Why fragile |
|------|-------------|
| `plain_parser.cpp` | Plain text parse heuristics + callback wiring still complex and regression-prone without integration tests |
| `book.h:237-291` | Public API mixes metadata, rendering, parsing, fixed-layout, async reflow, MOBI deferred |
| `include/app/app.h` public section | Public fields (`ts`, `prefs`, `buttons`, `books`) still allow broad external mutation of App state |
| `epub.cpp` | 1328 lines, zero tests, most-used format |
| `main.cpp` global `app` | Undefined behavior if accessed before init or after destruction |

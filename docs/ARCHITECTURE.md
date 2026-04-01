# Architecture

High-level architecture of 3dslibris as of v2.0.0. This document describes the current structure, module responsibilities, and known design trade-offs.

## Module map

```
source/
├── main.cpp                    # Entry point: 3DS bootstrap, service init, App lifecycle
├── app/                        # Application state machine + orchestration
│   ├── app.cpp                 # Main loop, mode routing, job queue, warmup
│   └── status_layout_utils.cpp # Status bar layout
├── library/                    # Library/browser screen
│   ├── app_browser.cpp         # Book grid, cover cache, metadata jobs
│   └── browser_*.h             # Cover cache, job queue, warmup utilities
├── reader/                     # Book reader screen
│   ├── app_book.cpp            # Open/reopen flow, input routing, deferred relayout
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
│   │   ├── book_io.cpp         # TXT/RTF/ODT/MOBI dispatch + MOBI cache
│   │   ├── buffered_status_log.cpp
│   │   ├── epub_image_utils.cpp
│   │   ├── file_read_utils.cpp
│   │   ├── page_cache_utils.cpp
│   │   ├── xml_parse_utils.cpp
│   │   ├── pdf_view_utils.cpp  # Shared MuPDF viewport/navigation
│   │   ├── fixed_layout_viewport_utils.h
│   │   └── rtf_control_word_utils.h
│   ├── epub/                   # EPUB2/EPUB3 parser
│   │   └── epub.cpp            # Full EPUB parsing + page cache
│   ├── fb2/                    # FictionBook 2 parser
│   │   └── fb2.cpp
│   ├── mobi/                   # MOBI/KF8 parser
│   │   ├── mobi.cpp            # Cover extraction
│   │   ├── mobi_cover_meta_cache.cpp
│   │   ├── mobi_heading_markers.cpp
│   │   ├── mobi_markup_tag.cpp
│   │   ├── mobi_position_map.cpp
│   │   ├── mobi_record_decode.cpp
│   │   ├── mobi_record_scan.cpp
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
  └─ main loop (input → mode dispatch → draw → frame swap)
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
       ├─ MOBI  → book_io.cpp + mobi/*.cpp (PDB records → pages)
       ├─ TXT   → book_io.cpp (raw text → pages)
       ├─ RTF   → book_io.cpp (RTF control words → pages)
       ├─ ODT   → book_io.cpp (minizip + expat → pages)
       ├─ PDF   → mupdf/*.cpp (MuPDF → display lists → bitmaps)
       ├─ CBZ   → cbz/*.cpp + mupdf/*.cpp (zip → images → bitmaps)
       └─ XPS   → mupdf/*.cpp (MuPDF → display lists → bitmaps)
```

Reflowable formats (EPUB, FB2, MOBI, TXT, RTF, ODT) produce `Page` objects with text buffers and layout tokens. Fixed-layout formats (PDF, CBZ, XPS) produce bitmaps via MuPDF.

## Known design trade-offs

### 1. App is a God Object
`App` handles state machine, input routing, job queue, cover warmup, screen drawing, settings, and menu management. This is a legacy from dslibris that has grown organically.

**Impact:** Every new screen or feature requires modifying App. Hard to test in isolation.

**Future direction:** Extract controllers (LibraryController, ReaderController, SettingsController) and reduce App to a state machine that delegates.

### 2. Book mixes model, parsing, and rendering
`Book` contains metadata (good), but also parsing methods (`Open()`, `Parse()`, `Index()`) and rendering methods (`DrawCurrentView()`, `DrawCurrentMuPdfView()`). It also holds an `App*` pointer creating a circular dependency.

**Impact:** Cannot reuse Book outside this project. Format-specific state (MuPdfState, CbzState) lives in the domain model.

**Future direction:** Separate Book (pure model) from BookParser (format-specific) and BookRenderer (format-specific).

### 3. book_io.cpp is a monolith (5369 lines)
Handles TXT, RTF, ODT, and MOBI parsing dispatch plus MOBI page cache serialization.

**Impact:** Any change to any of these formats requires navigating a massive file. High risk of accidental breakage.

**Future direction:** Split into per-format loaders and a separate cache module.

### 4. shared/ was a catch-all (now cleaned up)
Previously contained 19 headers of unrelated utilities. Now reduced to 4 genuinely cross-cutting modules:
- `app_flow_utils` — format detection, path conversion
- `text_layout_utils` — text layout helpers
- `text_unicode_utils` — Unicode utilities
- `utf8_utils` — UTF-8 encoding/decoding

## Build system

- **Target:** `3dslibris.3dsx` (homebrew) and `3dslibris.cia` (installable)
- **Toolchain:** devkitARM (ARM11 + ARM9 for New 3DS syscore)
- **Docker:** `docker/Dockerfile.cia` for reproducible builds
- **CI:** GitHub Actions (`.github/workflows/ci.yml`)
- **Tests:** Host-compiled C++ tests in `tests/` run via shell scripts

## Path constants

All SD card paths are centralized in `include/path_utils.h` under the `paths::` namespace. If the directory layout changes, update constants there only.

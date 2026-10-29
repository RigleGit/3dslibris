# Fixed-layout GPU renderer

CBZ, PDF/XPS interactive rasters and the EPUB/MOBI/FB2 image viewer share a
citro2d/citro3d renderer for the physical top screen. The lower screen and all
other modes retain their software renderer.

## Before and after

| Path | Previous behaviour | GPU path |
| --- | --- | --- |
| Top-screen pan | CPU crop, scale, orientation conversion and framebuffer copy | Reuse resident textures and draw visible quads |
| Wide top | CPU native-density blit, software shadow and BGR888 conversion | Native 240×800 render target; logical UI geometry is unchanged |
| CBZ/PDF raster | CPU pixels remain resident | Keep metadata and bottom preview; release main CPU pixels after upload and queue acceptance |
| Inline image raster | CPU pixels remain resident | CPU pixels still retained for decoder/control/fallback contracts |
| Inline overview pan | Blit the complete preview before drawing the viewport | Restore old viewport border strips, then draw the new outline |
| Other UI | Software | Software |

Decoding and rasterization still run on the CPU. Moving beyond the current PDF
raster's region still requires MuPDF to produce a new raster.

PDF zoom changes reuse a resident raster when it covers the new viewport and
contains enough samples for the requested display density, including Wide.
The raster's source generation stays unchanged, avoiding another texture upload.

CBZ also keeps an accepted lower-resolution decode when memory prevents the
requested zoom. Pan and zoom within that attempted range scale the resident
source directly, without repeating the decode or allocating a second bitmap.

## Ownership and limits

- `Text` owns the lazy GPU context and shuts it down before `gfxExit`.
- Per-source caches own textures; generations distinguish replacements even
  when dimensions, page and zoom are unchanged. A failed generation is not
  uploaded again every frame.
  Queue failures retain the CPU fallback for the current source. Navigation,
  source replacement and geometry/Wide changes permit another GPU attempt;
  reconstructing CPU pixels after a queue failure does not start a retry loop.
- Texture dimensions are powers of two, 8–1024 pixels. The maximum interior
  is 1022 pixels, with a one-pixel neighbour halo on each edge. Exterior page
  edges and unused padding are clamped.
  When the usual grid would exceed the byte cap, smaller remainder tiles are
  tried before falling back (2048×1536 uses about 6.055 MiB instead of 8.031 MiB).
- Each cache is limited to 8 MiB and 63 tiles. VRAM is attempted first; Linear
  fallback preserves a 1 MiB reserve. VRAM uploads group copies in at most
  256 KiB of staging, falling back to 64 KiB under memory pressure.
  Actual allocation failure also falls back to software.
- The offscreen target is not linked to a display output. `App::PresentIfDirty`
  flushes CPU writes before the GPU transfer, then performs the existing swap.
  GPU draw frames use citro3d's full Linear flush without a redundant framebuffer
  flush; software and retained-target-only frames keep their CPU flush.
  The main loop retains its existing VBlank wait.
- Both physical backbuffers track the retained target generation. Bottom-only
  changes cannot restore an obsolete software top image.
- Partial software overlays snapshot the last GPU-produced framebuffer into
  the existing software surfaces once, on transition. Resident pan does not
  perform this readback.
  Replacing a source keeps only its already-rendered target and cancels pending
  draws without a CPU snapshot. Later software overlays still recover pixels.
  Full redraws on orientation/Wide changes and library entry discard the old
  GPU image. A failed Wide allocation preserves it instead. Target recreation
  releases the stale background before reserving VRAM for the new target.
- All GPU operations are main-thread-only. Image-viewer suspend only flags
  GPU cleanup; normal execution performs it after resume. No workers are enabled.

## Diagnostics and verification

Build with `make DISABLE_FIXED_LAYOUT_GPU=1` to exercise the software path
(use `make clean-build` when switching). Debug upload events report duration, success, dimensions and
planned texture bytes through the existing fixed-layout performance queue.

Host tests cover tile bytes/halos, viewport coverage, retained preview pixels,
viewer controls and the existing format paths. A substituted SDK boundary also
exercises the production cache and App presenter: resource lifetime, grouped
uploads, low-memory fallback, CPU flush ordering and FrameBegin failure.
Tests also cover CPU fallback past GPU-only PDF rasters, accepted CBZ decode
fallback reuse, target resize under allocation pressure and Wide rollback.
These tests do not emulate the PICA200.
Physical Old/New 3DS tests remain necessary for colour/orientation, bilinear
seams, Wide, HOME/sleep/wake and repeated page/open/close cycles in CIA and 3DSX.

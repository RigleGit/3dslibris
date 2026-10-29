# Fixed-layout GPU renderer

CBZ, PDF/XPS interactive rasters and the EPUB/MOBI/FB2 image viewer share a
citro2d/citro3d renderer for the physical top screen. The lower screen and all
other modes retain their software renderer.

## Before and after

| Path | Previous behaviour | GPU path |
| --- | --- | --- |
| Top-screen pan | CPU crop, scale, orientation conversion and framebuffer copy | Reuse resident textures and draw visible quads |
| Wide top | CPU native-density blit, software shadow and BGR888 conversion | Native 240×800 render target; logical UI geometry is unchanged |
| CBZ/PDF raster | Full RGB565 intermediate plus decoded pixmap | Upload decoded RGB/gray samples directly; retain metadata, textures and the small filtered preview |
| Inline image raster | CPU pixels remain resident | Release CPU pixels after preview and GPU queue acceptance; reconstruct once if software is needed |
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

## Loading and CPU memory

The synchronous PDF/XPS and CBZ draw paths lend MuPDF's decoded pixmap to an
optional consumer before dropping it. The consumer first builds the bottom
preview, using the same RGB565 quantization and bilinear sampling as before,
then converts directly into bounded Morton upload chunks. It never allocates a
complete intermediate RGB565 bitmap on successful upload. If upload is rejected,
the decoder converts the still-live pixmap into the existing CPU bitmap. If queue
acceptance subsequently fails, the draw owner reconstructs the CPU source once
and latches software fallback for that source. Incremental/deferred composition
and callers without a main-thread `Text` retain their original CPU output.

The inline viewer also releases its main bitmap after queue acceptance, provided
its compact preview is ready at the current geometry. Controls and zoom admission
use metadata plus CPU/GPU validity. A failed larger decode preserves the old GPU
source. Preview geometry/background changes recover pixels when needed; suspend
continues to defer texture cleanup to normal main-thread execution. The compressed
resource cache stays capped at 1 MiB. The decoder context remains temporary.

Raster cover thumbnails use the existing bounded image decoder on 3DS, including
MuPDF JPEG DCT subsampling, a 16 MiB decoder allocation limit and first-frame GIF
handling. Only a small RGB565 thumbnail is committed to `Book` ownership. PNG
still needs pixel decoding within the budget; this does not add GPU work to the
library. Transparent cover pixels are composed onto white paper.

These changes reduce conversion passes and transient/retained CPU memory. They
have not yet been timed on physical Old/New 2DS; earlier Azahar page-load numbers
predate these changes and cannot establish their speedup.

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
  edges are clamped; complete padding blocks outside the halo are zero and
  never sample the source. Swizzling reads contiguous rows inside 8×8 blocks,
  with bounds hoisted out of the per-texel loop.
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
`pdf.gpu_source` and `cbz.decode_gpu_source` distinguish fused GPU preparation
from full CPU RGB565 conversion; `IMAGE recover_cpu` records exceptional viewer
reconstruction, while ordinary pan/covered zoom should not produce it.

Host tests cover tile bytes/halos, viewport coverage, retained preview pixels,
viewer controls and the existing format paths. A substituted SDK boundary also
exercises the production cache and App presenter: resource lifetime, grouped
uploads, low-memory fallback, CPU flush ordering and FrameBegin failure.
Tests also cover CPU fallback past GPU-only PDF rasters, accepted CBZ decode
fallback reuse, target resize under allocation pressure and Wide rollback.
Native MuPDF tests (`MUPDF_HOST_LIB_DIR`) additionally exercise accepted/rejected
pixmap consumers, native-density samples and a 2400×3600 JPEG cover. Tile tests
compare fused RGB/gray bytes and previews with the existing RGB565 path, including
padded strides. Viewer tests cover GPU-only controls, failed zoom preservation,
one-time recovery and deferred APT cleanup. These tests do not emulate the PICA200.
Validation on 2026-10-29: `make test-host` with native MuPDF libraries passed
140/142 suites, with zero failures and two host ARM-toolchain skips. Docker
successfully built release/debug 3DSX and CIA packages. Physical Old/New 3DS
tests remain necessary for colour/orientation, bilinear
seams, Wide, HOME/sleep/wake and repeated page/open/close cycles in CIA and 3DSX.

### Physical Old/New 2DS follow-up

Use both CIA and 3DSX, recording which model/runtime produced each debug log:

1. PDF/XPS and CBZ: open, turn pages repeatedly, pan in both axes, zoom out/in;
   confirm sharp detail, correct colours, no seams and no repeated uploads while
   the source remains sufficient. Compare page-loading timings with the earlier
   logs using the same document/page/zoom/orientation.
2. EPUB/MOBI/FB2 full-screen images: pan and zoom after CPU pixels are released;
   check the overview rectangle, controls, fit/back/settings/library and a failed
   zoom on a memory-heavy image. `bitmap_bytes=0` during resident GPU interaction
   is expected; a CPU recovery should occur only on fallback/invalidation.
3. Test all orientations and Wide where supported. Open menus, return to software,
   press HOME, sleep/wake, then continue panning and turning pages.
4. Reopen/close documents and revisit a library with large JPEG sidecars. Check
   cover colours and that memory does not grow across repeated cycles.

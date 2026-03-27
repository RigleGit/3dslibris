#!/bin/sh

set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="$ROOT/build-tests/mupdf"
PDFDIR="$OUTDIR/pdf"
mkdir -p "$OUTDIR" "$PDFDIR"

cat >"$OUTDIR/test_mupdf_link.c" <<'EOF'
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <stdio.h>

int main(void) {
  fz_context *ctx = NULL;
  fz_document *doc = NULL;
  fz_outline *outline = NULL;
  fz_page *page = NULL;
  fz_pixmap *pix = NULL;
  fz_device *dev = NULL;
  fz_matrix ctm = fz_scale(1.0f, 1.0f);
  fz_rect bounds;
  fz_irect bbox;
  int page_count = 0;

  ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  if (!ctx)
    return 2;

  fz_register_document_handlers(ctx);
  doc = fz_open_document(ctx, "dummy.pdf");
  page_count = fz_count_pages(ctx, doc);
  outline = fz_load_outline(ctx, doc);
  page = fz_load_page(ctx, doc, 0);
  bounds = fz_bound_page(ctx, page);
  bbox = fz_round_rect(bounds);
  pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, NULL, 0);
  dev = fz_new_draw_device(ctx, fz_identity, pix);
  fz_run_page(ctx, page, dev, ctm, NULL);
  fz_close_device(ctx, dev);
  fz_drop_device(ctx, dev);
  fz_drop_pixmap(ctx, pix);
  fz_drop_page(ctx, page);
  fz_drop_outline(ctx, outline);
  fz_drop_document(ctx, doc);
  fz_drop_context(ctx);
  return page_count;
}
EOF

sh "$ROOT/scripts/build_mupdf_minimal.sh"

/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc \
  -D__3DS__ -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft \
  -I"$ROOT/third_party/mupdf/include" \
  "$OUTDIR/test_mupdf_link.c" \
  "$ROOT/third_party/mupdf/build/3ds-minimal/libmupdf.a" \
  "$ROOT/third_party/mupdf/build/3ds-minimal/libmupdf-third.a" \
  -L/opt/devkitpro/libctru/lib \
  -L/opt/devkitpro/portlibs/3ds/lib \
  -specs=3dsx.specs -lctru -lm \
  -o "$OUTDIR/test_mupdf_link.elf"

test -f "$OUTDIR/test_mupdf_link.elf"

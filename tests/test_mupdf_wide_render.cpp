#include "formats/mupdf/mupdf_render.h"
#include <cassert>
#include <cstdio>
#include <new>
#include <string>

u16 g_gray_to_rgb565[256];
void EnsureGrayLut() {}
static bool reject_native_pixels = false;
void *operator new[](size_t bytes, const std::nothrow_t &) noexcept {
  if (reject_native_pixels) { reject_native_pixels = false; return nullptr; }
  return ::operator new[](bytes);
}

static void WriteStripes(const char *path) {
  std::string stream = "0 g\n";
  for (int y = 0; y < 400; ++y)
    stream += "0 " + std::to_string(y) + " 240 0.5 re f\n";
  const std::string objects[] = {
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 240 400] /Resources << >> /Contents 4 0 R >>",
    "<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "endstream"
  };
  FILE *file = fopen(path, "wb"); assert(file);
  fprintf(file, "%%PDF-1.4\n");
  long offsets[4];
  for (int i = 0; i < 4; ++i) {
    offsets[i] = ftell(file);
    fprintf(file, "%d 0 obj\n%s\nendobj\n", i + 1, objects[i].c_str());
  }
  const long xref = ftell(file);
  fprintf(file, "xref\n0 5\n0000000000 65535 f \n");
  for (long offset : offsets) fprintf(file, "%010ld 00000 n \n", offset);
  fprintf(file, "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref);
  assert(fclose(file) == 0);
}

int main(int argc, char **argv) {
  assert(argc == 2); WriteStripes(argv[1]);
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT); assert(ctx);
  fz_register_document_handlers(ctx);
  fz_document *doc = fz_open_document(ctx, argv[1]); assert(doc);
  float width = 0, height = 0;
  RenderedMuPdfBitmap normal, native;
  assert(RenderMuPdfBitmap(ctx, doc, 0, 1, &normal, &width, &height));
  assert(normal.width == 240 && normal.height == 400 && width == 240 && height == 400);
  assert(RenderMuPdfBitmap(ctx, doc, 0, 1, &native, &width, &height,
      NULL, NULL, NULL, NULL, NULL, 1, 2));
  assert(native.width == 240 && native.height == 800 && native.native_pixels);
  for (int y = 0; y < 800; ++y)
    assert(native.PixelData()[y * 240 + 120] == (y & 1 ? 0 : 0xFFFF));
  // Transfer ownership to the real cache without another allocation/copy.
  Book::MuPdfState::BitmapCache cache;
  const u16 *address = native.PixelData();
  StoreBitmapCache(&cache, 0, 0, 0, 0, 1, 1, &native);
  assert(cache.PixelData() == address && !native.native_pixels);
  assert(BitmapCacheValid(cache, 0) && cache.PixelCount() == 240u * 800u);
  ResetBitmapCache(&cache); assert(!BitmapCacheValid(cache, 0));
  assert(RenderMuPdfBitmap(ctx, doc, 0, 1, &native, &width, &height,
      NULL, NULL, NULL, NULL, NULL, 2, 1));
  assert(native.width == 480 && native.height == 400);
  RenderedMuPdfBitmap capped;
  assert(RenderMuPdfBitmap(ctx, doc, 0, 3, &capped, &width, &height,
      NULL, NULL, NULL, NULL, NULL, 1, 2));
  assert(capped.width == 720 && capped.height == 1200);
  // An optional allocation failure is handled, leaving the normal renderer
  // usable and the real MuPDF context clean enough for repeated teardown.
  reject_native_pixels = true;
  RenderedMuPdfBitmap failed;
  assert(!RenderMuPdfBitmap(ctx, doc, 0, 1, &failed, &width, &height,
      NULL, NULL, NULL, NULL, NULL, 1, 2));
  assert(failed.width == 0 && failed.PixelCount() == 0);
  assert(fz_caught(ctx) == FZ_ERROR_NONE);
  assert(RenderMuPdfBitmap(ctx, doc, 0, 1, &failed, &width, &height));
  assert(failed.width == 240 && failed.height == 400);
  fz_drop_document(ctx, doc); fz_drop_context(ctx);
  puts("PASS: real PDF native stripes, density axes, cap, cache ownership and allocation recovery");
}

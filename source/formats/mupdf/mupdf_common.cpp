// SPDX-License-Identifier: AGPL-3.0-or-later

#include "formats/mupdf/mupdf_common.h"

#include "shared/color_utils.h"

#include <3ds.h>

namespace {

LightLock s_mupdf_locks[FZ_LOCK_MAX];
bool kGrayLutReady = false;

void MuPdfLockAcquire(void *, int lock) { LightLock_Lock(&s_mupdf_locks[lock]); }
void MuPdfLockRelease(void *, int lock) { LightLock_Unlock(&s_mupdf_locks[lock]); }

} // namespace

fz_locks_context g_mupdf_locks_ctx;
u16 g_gray_to_rgb565[256];

void InitMuPdfLocks() {
  for (int i = 0; i < FZ_LOCK_MAX; i++)
    LightLock_Init(&s_mupdf_locks[i]);
  g_mupdf_locks_ctx.user = NULL;
  g_mupdf_locks_ctx.lock = MuPdfLockAcquire;
  g_mupdf_locks_ctx.unlock = MuPdfLockRelease;
}

void EnsureGrayLut() {
  if (kGrayLutReady)
    return;
  for (int g = 0; g < 256; g++) {
    g_gray_to_rgb565[g] = (u16)(((u16)(g >> 3) << 11) |
                             ((u16)(g >> 2) << 5) |
                             (u16)(g >> 3));
  }
  kGrayLutReady = true;
}

bool DetectNew3ds() {
  bool is_new_3ds = false;
  APT_CheckNew3DS(&is_new_3ds);
  return is_new_3ds;
}

u16 RGB565FromRgb8(unsigned char r, unsigned char g, unsigned char b) {
  return (u16)(((u16)(r >> 3) << 11) | ((u16)(g >> 2) << 5) | (u16)(b >> 3));
}

void RGB565ToRgb8(u16 pixel, int *r, int *g, int *b) {
  if (!r || !g || !b)
    return;
  UnpackRgb565(pixel, r, g, b);
}

bool IsMostlyWhite(u16 pixel) {
  int r = 0;
  int g = 0;
  int b = 0;
  RGB565ToRgb8(pixel, &r, &g, &b);
  return (r >= 248 && g >= 248 && b >= 248);
}

float ComputeFitScale(float page_width, float page_height, int target_width,
                      int target_height) {
  if (page_width <= 0.0f || page_height <= 0.0f || target_width <= 0 ||
      target_height <= 0) {
    return 1.0f;
  }
  const float sx = (float)target_width / page_width;
  const float sy = (float)target_height / page_height;
  const float scale = std::min(sx, sy);
  return (scale > 0.0f) ? scale : 1.0f;
}

float ComputeEffectiveMuPdfZoom(app_flow_utils::MuPdfDocumentKind kind,
                                int zoom_index) {
  return pdf_view_utils::ZoomForIndex(zoom_index) *
         app_flow_utils::GetMuPdfReadingBaseZoom(kind);
}

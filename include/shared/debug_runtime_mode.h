#pragma once

namespace debug_runtime {

// Build-time switch for diagnosing the software path without changing prefs.
inline bool DisableFixedLayoutGpuRenderer() {
#ifdef DSLIBRIS_DISABLE_FIXED_LAYOUT_GPU
  return true;
#else
  return false;
#endif
}

inline bool DisableEpubPageResidency() {
#ifdef DSLIBRIS_EPUB_FULL_CACHE
  return true;
#else
  return false;
#endif
}

// Stability-first runtime: keep all background/deferred work disabled in both
// normal and debug builds until the fixed-layout and MOBI paths are proven on
// hardware again.
inline bool BackgroundWorkersDisabled() { return true; }

inline bool BrowserWarmupDisabled() { return false; }

inline bool ForceSynchronousCbzDecode() { return true; }

inline bool ForceSynchronousMuPdfRender() { return true; }

} // namespace debug_runtime

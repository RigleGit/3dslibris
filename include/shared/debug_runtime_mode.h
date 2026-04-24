#pragma once

namespace debug_runtime {

// Stability-first runtime: keep all background/deferred work disabled in both
// normal and debug builds until the fixed-layout and MOBI paths are proven on
// hardware again.
inline bool BackgroundWorkersDisabled() { return true; }

inline bool BrowserWarmupDisabled() { return false; }

inline bool ForceSynchronousCbzDecode() { return true; }

inline bool ForceSynchronousMuPdfRender() { return true; }

} // namespace debug_runtime

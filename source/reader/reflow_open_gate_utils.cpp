#include "reader/reflow_open_gate_utils.h"

namespace reader {

bool ShouldUseAsyncReflowOpen(bool uses_text_layout_settings) {
  // All formats use the async worker on New 3DS — PDF/CBZ/XPS included.
  // Fixed-layout parsers have no in-parser abort mechanism, but running them
  // on core 1 keeps the main thread free to respond to HOME button events.
  // The OS suspends all threads when HOME is active, so the worker is safe.
  (void)uses_text_layout_settings;
  return true;
}

} // namespace reader

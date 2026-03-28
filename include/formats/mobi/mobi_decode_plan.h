#ifndef DSLIBRIS_FORMATS_MOBI_MOBI_DECODE_PLAN_H
#define DSLIBRIS_FORMATS_MOBI_MOBI_DECODE_PLAN_H

#include <stddef.h>

namespace mobi_decode_plan {

static const size_t kDeferredTocFinalizeMinBytes = 1024 * 1024;

struct Plan {
  bool defer_toc_finalize;
  bool capture_toc_metadata;
  bool retain_markup_utf8;
};

inline Plan Build(size_t text_bytes) {
  Plan plan;
  plan.defer_toc_finalize = text_bytes >= kDeferredTocFinalizeMinBytes;
  // Keep metadata from the initial markup pass even when TOC finalization is
  // deferred, so large MOBIs do not pay for a second full markup->text scan.
  plan.capture_toc_metadata = true;
  plan.retain_markup_utf8 = !plan.capture_toc_metadata;
  return plan;
}

} // namespace mobi_decode_plan

#endif // DSLIBRIS_FORMATS_MOBI_MOBI_DECODE_PLAN_H

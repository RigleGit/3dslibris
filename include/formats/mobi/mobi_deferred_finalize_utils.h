#ifndef DSLIBRIS_SHARED_MOBI_DEFERRED_FINALIZE_UTILS_H
#define DSLIBRIS_SHARED_MOBI_DEFERRED_FINALIZE_UTILS_H

namespace mobi_deferred_finalize_utils {

enum class FinalizeStage {
  ContinuePaging = 0,
  BuildMetadata,
  LoadStructuredToc,
  ApplyToc,
  SaveCache,
  Done,
};

inline FinalizeStage NextFinalizeStage(bool stream_completed,
                                       bool toc_metadata_ready,
                                       bool structured_toc_loaded,
                                       bool toc_applied, bool cache_saved,
                                       bool finalized) {
  (void)finalized;
  if (!stream_completed)
    return FinalizeStage::ContinuePaging;
  if (!toc_metadata_ready)
    return FinalizeStage::BuildMetadata;
  if (!structured_toc_loaded)
    return FinalizeStage::LoadStructuredToc;
  if (!toc_applied)
    return FinalizeStage::ApplyToc;
  if (!cache_saved)
    return FinalizeStage::SaveCache;
  return FinalizeStage::Done;
}

} // namespace mobi_deferred_finalize_utils

#endif

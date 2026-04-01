#pragma once

namespace reflow_cache_save_utils {

inline bool ShouldDeferAsyncOpenCacheSave(bool save_cache_requested,
                                          bool async_open_pending) {
  return save_cache_requested && async_open_pending;
}

inline bool ShouldFlushDeferredCacheSaveOnClose(bool pending_cache_save,
                                                bool async_open_pending,
                                                unsigned int page_count) {
  return pending_cache_save && !async_open_pending && page_count > 0;
}

} // namespace reflow_cache_save_utils

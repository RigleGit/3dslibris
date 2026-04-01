#ifndef DSLIBRIS_SHARED_DEFERRED_RELAYOUT_UTILS_H
#define DSLIBRIS_SHARED_DEFERRED_RELAYOUT_UTILS_H

#include <list>

#include "book/layout_reflow.h"

namespace deferred_relayout_utils {

struct OpenRelayoutPlan {
  bool has_remap;
  bool defer_final_remap;
  int mapped_position;
  std::list<int> mapped_bookmarks;

  OpenRelayoutPlan()
      : has_remap(false), defer_final_remap(false), mapped_position(0),
        mapped_bookmarks() {}
};

inline OpenRelayoutPlan
BuildOpenRelayoutPlan(bool needs_relayout, bool has_deferred_parse,
                      int old_page_count, int old_position,
                      int current_page_count,
                      const std::list<int> &old_bookmarks) {
  OpenRelayoutPlan plan;
  if (!needs_relayout)
    return plan;

  plan.has_remap = true;
  plan.defer_final_remap = has_deferred_parse;
  plan.mapped_position = layout_reflow::RemapPageIndexApprox(
      old_position, old_page_count, current_page_count);
  plan.mapped_bookmarks = layout_reflow::RemapBookmarksApprox(
      old_bookmarks, old_page_count, current_page_count);
  return plan;
}

inline bool ShouldApplyFinalDeferredRelayout(bool pending,
                                             bool has_deferred_parse,
                                             int current_position,
                                             int initial_position) {
  return pending && !has_deferred_parse && current_position == initial_position;
}

inline bool ShouldCancelFinalDeferredRelayout(bool pending, int current_position,
                                              int initial_position) {
  return pending && current_position != initial_position;
}

} // namespace deferred_relayout_utils

#endif

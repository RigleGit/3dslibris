#include "formats/mobi/mobi_cleanup_policy.h"

namespace mobi_cleanup_policy {

bool ShouldRunPostNormalizeCleanup(bool have_html_map,
                                   bool line_wrap_fix_applied) {
  return have_html_map || line_wrap_fix_applied;
}

} // namespace mobi_cleanup_policy

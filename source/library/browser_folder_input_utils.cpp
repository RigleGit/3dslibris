#include "library/browser_folder_input_utils.h"

namespace browser_folder_input_utils {

bool ShouldLeaveFolder(uint32_t keys, uint32_t back_key, uint32_t start_key) {
  return (keys & (back_key | start_key)) != 0;
}

} // namespace browser_folder_input_utils

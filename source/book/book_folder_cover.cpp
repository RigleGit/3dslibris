#include "book/book.h"
#include "library/cover_override_utils.h"

#include <sys/stat.h>

const std::string &Book::GetBrowserFolderCoverPath() const {
  if (!browser_folder_entry || browser_folder_cover_checked_)
    return browser_folder_cover_path;
  browser_folder_cover_checked_ = true;
  const std::vector<std::string> candidates =
      cover_override_utils::BuildStemCandidates(browser_folder_path);
  for (size_t i = 0; i < candidates.size(); ++i) {
    struct stat st;
    if (stat(candidates[i].c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      browser_folder_cover_path = candidates[i];
      break;
    }
  }
  return browser_folder_cover_path;
}

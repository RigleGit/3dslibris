#include "library/cover_override_utils.h"

namespace cover_override_utils {

std::vector<std::string> BuildStemCandidates(const std::string &stem_path) {
  std::vector<std::string> result;
  if (stem_path.empty() || stem_path[stem_path.size() - 1] == '/')
    return result;
  const char *extensions[] = {".jpg", ".jpeg", ".png", ".JPG", ".JPEG", ".PNG"};
  for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
    result.push_back(stem_path + extensions[i]);
  return result;
}

std::vector<std::string> BuildFolderCandidates(const std::string &folder_path) {
  std::string folder = folder_path;
  while (!folder.empty() && folder[folder.size() - 1] == '/')
    folder.erase(folder.size() - 1);
  if (folder.empty())
    return std::vector<std::string>();
  std::vector<std::string> result = BuildStemCandidates(folder);
  const size_t slash = folder.find_last_of('/');
  const std::string base =
      slash == std::string::npos ? folder : folder.substr(slash + 1);
  const std::vector<std::string> inside =
      BuildStemCandidates(folder + "/" + base);
  result.insert(result.end(), inside.begin(), inside.end());
  return result;
}

std::vector<std::string> BuildBookCandidates(const std::string &book_path) {
  const size_t slash = book_path.find_last_of('/');
  const size_t dot = book_path.find_last_of('.');
  const size_t stem_end = dot != std::string::npos &&
                                  (slash == std::string::npos || dot > slash)
                              ? dot
                              : book_path.size();
  return BuildStemCandidates(book_path.substr(0, stem_end));
}

} // namespace cover_override_utils

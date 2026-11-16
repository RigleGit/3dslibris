#pragma once

#include <string>
#include <vector>

namespace cover_override_utils {

std::vector<std::string> BuildBookCandidates(const std::string &book_path);
std::vector<std::string> BuildStemCandidates(const std::string &stem_path);
// Sibling "Foo.<ext>" candidates first, then "Foo/Foo.<ext>" inside the folder.
std::vector<std::string> BuildFolderCandidates(const std::string &folder_path);

} // namespace cover_override_utils

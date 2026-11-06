/*
    3dslibris - path_utils.h
    Generic path/URL manipulation utilities (project-agnostic).
    For SD card path constants, see path_constants.h.
*/

#ifndef PATH_UTILS_H
#define PATH_UTILS_H

#include <algorithm>
#include <stdio.h>
#include <string>
#include <vector>

#include <sys/stat.h>
#ifdef __3DS__
// libctru's archive.h has no extern "C" guard; without it the C++ build looks
// for a mangled archive_getmtime and fails to link.
extern "C" {
#include <3ds/archive.h>
}
#endif

namespace paths {
// libctru stat leaves st_mtime zero. archive_getmtime already returns UNIX
// seconds; immutable/unsupported archives retain stat's fallback timestamp.
inline bool StatWithMtime(const char *name, struct stat *info) {
  if (!name || !info || ::stat(name, info) != 0)
    return false;
#ifdef __3DS__
  u64 modified = 0;
  if (archive_getmtime(name, &modified) == 0)
    info->st_mtime = (time_t)modified;
#endif
  return true;
}
} // namespace paths

// --- URL decoding ---

inline std::string UrlDecode(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); i++) {
    if (input[i] == '%' && i + 2 < input.size()) {
      unsigned int value = 0;
      if (sscanf(input.substr(i + 1, 2).c_str(), "%x", &value) == 1) {
        out.push_back((char)value);
        i += 2;
        continue;
      }
    }
    out.push_back(input[i]);
  }
  return out;
}

// --- Path normalization (resolve `.`, `..`, backslashes, leading `/`) ---

inline std::string NormalizePath(const std::string &path) {
  std::string in = path;
  std::replace(in.begin(), in.end(), '\\', '/');
  while (!in.empty() && in[0] == '/')
    in.erase(in.begin());

  std::vector<std::string> parts;
  std::string cur;
  for (size_t i = 0; i <= in.size(); i++) {
    if (i == in.size() || in[i] == '/') {
      if (cur == "..") {
        if (!parts.empty())
          parts.pop_back();
      } else if (!cur.empty() && cur != ".") {
        parts.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(in[i]);
    }
  }

  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i)
      out.push_back('/');
    out += parts[i];
  }
  return out;
}

// --- Basename extraction ---

inline std::string BasenamePath(const std::string &path) {
  if (path.empty())
    return "";
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return path;
  if (slash + 1 >= path.size())
    return "";
  return path.substr(slash + 1);
}

// --- Strip #fragment and ?query ---

inline std::string StripFragmentAndQuery(const std::string &path) {
  size_t stop = path.find_first_of("#?");
  if (stop == std::string::npos)
    return path;
  return path.substr(0, stop);
}

// --- Resolve a relative path against a base file path ---

inline std::string ResolveRelativePath(const std::string &base_file,
                                       const std::string &ref_raw) {
  std::string ref = UrlDecode(ref_raw);
  if (ref.empty())
    return "";
  if (ref.find("://") != std::string::npos)
    return "";

  if (ref[0] == '/') {
    return NormalizePath(ref);
  }

  std::string base = base_file;
  size_t slash = base.find_last_of('/');
  std::string folder =
      (slash == std::string::npos) ? "" : base.substr(0, slash + 1);
  return NormalizePath(folder + ref);
}

#endif // PATH_UTILS_H

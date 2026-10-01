#pragma once

#include <string>
#include <vector>

inline void GetFallbackFontSearchDirs(const std::string &configured_dir,
                                      const char *default_dir,
                                      std::vector<std::string> *dirs) {
  if (!dirs)
    return;
  dirs->clear();

  const std::string primary = configured_dir.empty() ? std::string(default_dir ? default_dir : "")
                                                     : configured_dir;
  dirs->push_back(primary);

  const std::string romfs_font_dir = std::string("romfs:/3ds/3dslibris/font");
  if (romfs_font_dir != primary)
    dirs->push_back(romfs_font_dir);
}

inline bool GlyphWithinContentRight(int sx, int content_right) {
  return sx < content_right;
}

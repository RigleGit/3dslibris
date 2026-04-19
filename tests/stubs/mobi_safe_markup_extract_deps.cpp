#include <cstdio>
#include <stdint.h>
#include <string>

bool mobi_extract_image_recindex(const std::string &tag,
                                 uint16_t *recindex_out) {
  if (!recindex_out)
    return false;
  unsigned int value = 0;
  if (std::sscanf(tag.c_str(), "img recindex=\"%u\"", &value) == 1) {
    *recindex_out = (uint16_t)value;
    return true;
  }
  if (std::sscanf(tag.c_str(), "img src=\"kindle:embed:%u", &value) == 1) {
    *recindex_out = (uint16_t)value;
    return true;
  }
  return false;
}

std::string mobi_inline_image_path(uint16_t recindex) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "mobi:img:%u", (unsigned)recindex);
  return std::string(buf);
}

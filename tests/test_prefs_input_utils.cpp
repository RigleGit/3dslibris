#include "settings/prefs_input_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

} // namespace

int main() {
  const uint32_t key_b = 1u << 0;
  const uint32_t key_select = 1u << 1;
  const uint32_t key_y = 1u << 2;
  const uint32_t key_start = 1u << 3;
  const uint32_t key_a = 1u << 4;

  ExpectTrue("B returns from browser prefs",
             prefs_input_utils::ShouldReturnFromPrefs(
                 key_b, false, key_b, key_select, key_y, key_start));
  ExpectTrue("START returns from browser prefs",
             prefs_input_utils::ShouldReturnFromPrefs(
                 key_start, false, key_b, key_select, key_y, key_start));
  ExpectFalse("A does not return from browser prefs",
              prefs_input_utils::ShouldReturnFromPrefs(
                  key_a, false, key_b, key_select, key_y, key_start));
  ExpectFalse("START keeps book prefs direct-library behavior separate",
              prefs_input_utils::ShouldReturnFromPrefs(
                  key_start, true, key_b, key_select, key_y, key_start));

  return 0;
}

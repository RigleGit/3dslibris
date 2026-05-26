#include "library/browser_folder_input_utils.h"

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
  const uint32_t key_start = 1u << 1;
  const uint32_t key_select = 1u << 2;

  ExpectTrue("B leaves folder",
             browser_folder_input_utils::ShouldLeaveFolder(key_b, key_b,
                                                           key_start));
  ExpectTrue("START leaves folder",
             browser_folder_input_utils::ShouldLeaveFolder(key_start, key_b,
                                                           key_start));
  ExpectFalse("SELECT does not leave folder",
              browser_folder_input_utils::ShouldLeaveFolder(key_select, key_b,
                                                            key_start));

  return 0;
}

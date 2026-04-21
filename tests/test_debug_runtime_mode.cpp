#include "shared/debug_runtime_mode.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &msg) {
  std::fprintf(stderr, "%s\n", msg.c_str());
  std::exit(1);
}

struct FlagExpectation {
  const char *label;
  bool actual;
  bool expected;
};

void CheckFlags(const FlagExpectation *flags, int count) {
  bool any_failed = false;
  for (int i = 0; i < count; ++i) {
    const FlagExpectation &f = flags[i];
    if (f.actual != f.expected) {
      std::fprintf(stderr, "%s: expected %s but got %s\n", f.label,
                   f.expected ? "true" : "false",
                   f.actual ? "true" : "false");
      any_failed = true;
    }
  }
  if (any_failed)
    std::exit(1);
}

} // namespace

int main() {
  // Document the intended runtime mode for each flag.
  // Update expected values here when a flag is re-enabled.
  FlagExpectation flags[] = {
      {"BackgroundWorkersDisabled", debug_runtime::BackgroundWorkersDisabled(), true},
      {"BrowserWarmupDisabled",     debug_runtime::BrowserWarmupDisabled(),     false},
      {"ForceSynchronousCbzDecode", debug_runtime::ForceSynchronousCbzDecode(), true},
      {"ForceSynchronousMuPdfRender", debug_runtime::ForceSynchronousMuPdfRender(), true},
  };
  CheckFlags(flags, 4);
  return 0;
}

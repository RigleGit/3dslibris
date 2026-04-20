#include "shared/debug_runtime_mode.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &msg) {
  std::fprintf(stderr, "%s\n", msg.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

} // namespace

int main() {
  ExpectTrue("background workers disabled",
             debug_runtime::BackgroundWorkersDisabled());
  ExpectTrue("browser warmup disabled",
             debug_runtime::BrowserWarmupDisabled());
  ExpectTrue("sync book open forced",
             debug_runtime::ForceSynchronousBookOpen());
  ExpectTrue("sync cbz decode forced",
             debug_runtime::ForceSynchronousCbzDecode());
  ExpectTrue("sync mupdf render forced",
             debug_runtime::ForceSynchronousMuPdfRender());
  ExpectTrue("sync mobi finalize forced",
             debug_runtime::ForceSynchronousMobiFinalize());
  return 0;
}

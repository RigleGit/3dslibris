#include "shared/boot_trace.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>

u64 boot_trace_test_time = 1000;

static std::string Read(const char *path) {
  std::ifstream file(path);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

int main() {
  const char *boot_path = "sdmc:/3dslibris_boot_trace.txt";
  const char *apt_path = "sdmc:/3dslibris_apt_trace.txt";
  mkdir("sdmc:", 0700);
  std::remove(boot_path);
  std::remove(apt_path);
  boot_trace::Boot("test startup");
  boot_trace::RecordAptHook(APTHOOK_ONSUSPEND);
  if (!Read(apt_path).empty()) {
    std::fprintf(stderr, "APT hook must not write to SD\n");
    return 1;
  }
  boot_trace_test_time = 5000;
  boot_trace::RecordAptHook(APTHOOK_ONRESTORE);
  boot_trace_test_time = 9000;
  boot_trace::FlushAptHooks();

#ifdef DSLIBRIS_DEBUG
  if (Read(boot_path).find("1000 test startup") == std::string::npos ||
      Read(apt_path).find("1000 APTHOOK_ONSUSPEND") == std::string::npos ||
      Read(apt_path).find("5000 APTHOOK_ONRESTORE") == std::string::npos) {
    std::fprintf(stderr, "Trace must preserve event time, not flush time\n");
    return 1;
  }
  const std::string before = Read(apt_path);
  boot_trace::FlushAptHooks();
  if (Read(apt_path) != before) {
    std::fprintf(stderr, "Flushing again must not duplicate events\n");
    return 1;
  }
#else
  struct stat st;
  if (stat(boot_path, &st) == 0 || stat(apt_path, &st) == 0) {
    std::fprintf(stderr, "Release must not create diagnostic trace files\n");
    return 1;
  }
#endif
  return 0;
}

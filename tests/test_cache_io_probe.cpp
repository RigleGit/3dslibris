#include "shared/cache_io_probe.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
static bool Cancel(void *p) {
  return ++*(unsigned *)p < 4;
}
int main() {
  char dir[] = "/tmp/3dslibris-io-probe-XXXXXX";
  assert(mkdtemp(dir));
  const std::string file = std::string(dir) + "/probe.tmp";
  const auto result =
      cache_io_probe::Run(file, nullptr, nullptr, nullptr, 65537);
  assert(result.ok && !result.cancelled);
  assert(result.read_bytes == 6 * 65537 && result.write_bytes == 6 * 65537);
  assert(result.read_calls == 16 && result.write_calls == 16);
  assert(access(file.c_str(), F_OK) != 0);
  unsigned calls = 0;
  const auto cancelled =
      cache_io_probe::Run(file, nullptr, Cancel, &calls, 65537);
  assert(cancelled.cancelled && !cancelled.ok &&
         access(file.c_str(), F_OK) != 0);
  assert(rmdir(dir) == 0);
  puts("cache I/O probe: real writes, reads, cancellation and cleanup passed");
}

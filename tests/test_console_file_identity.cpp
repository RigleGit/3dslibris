#include "formats/common/page_cache_utils.h"
#include <cassert>
#include <cstdio>
#include <unistd.h>
#include <utime.h>
static unsigned long long timestamp = 1600000100;
extern "C" int archive_getmtime(const char *, unsigned long long *out) {
  *out = timestamp; return 0;
}
int main() {
  char filename[]="/tmp/3dslibris-console-identity-XXXXXX";
  int fd=mkstemp(filename);assert(fd>=0);assert(write(fd,"book",4)==4);close(fd);
  utimbuf zero={0,0};assert(utime(filename,&zero)==0);
  const auto first=page_cache_utils::BuildLayoutParams(filename,12,0,0,0,0,0,0,0,0,"");
  assert(first.file_mtime==1600000100 && "console identity must use archive timestamp, not zero stat mtime");
  timestamp=1600000200;
  const auto replaced=page_cache_utils::BuildLayoutParams(filename,12,0,0,0,0,0,0,0,0,"");
  assert(first.file_size==replaced.file_size);
  assert(page_cache_utils::BuildPageCachePath("cache",".epc",filename,first)!=page_cache_utils::BuildPageCachePath("cache",".epc",filename,replaced));
  timestamp=946684800;utimbuf stable={1234,1234};assert(utime(filename,&stable)==0);
  const auto unsupported=page_cache_utils::BuildLayoutParams(filename,12,0,0,0,0,0,0,0,0,"");
  assert(unsupported.file_mtime==1234 && "an untouched archive timestamp must not replace stat's value");
  unlink(filename);
  puts("PASS: zero console stat mtime cannot hide same-size source replacement");
}

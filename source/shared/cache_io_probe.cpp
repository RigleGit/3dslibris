#include "shared/cache_io_probe.h"
#include "shared/status_reporter.h"
#include "shared/debug_log.h"
#include <3ds.h>
#include <algorithm>
#include <fcntl.h>
#include <new>
#include <unistd.h>
namespace cache_io_probe {
Stats Run(const std::string &path, IStatusReporter *reporter,
          bool (*poll)(void *), void *context, size_t bytes) {
  Stats stats;
  for (size_t size : {(size_t)16384, (size_t)65536, (size_t)262144}) {
    unsigned char *buffer = new (std::nothrow) unsigned char[size];
    if (!buffer)
      return stats;
    for (size_t i = 0; i < size; ++i)
      buffer[i] = (unsigned char)(i * 73 + 29);
    for (bool reserve : {false, true}) {
      const size_t reads_before = stats.read_calls,
                   writes_before = stats.write_calls;
      int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
      bool ok = fd >= 0;
      const u64 begin = osGetTime();
      if (ok && reserve)
        ok = ftruncate(fd, bytes) == 0;
      const u64 reserved = osGetTime();
      for (size_t offset = 0; ok && offset < bytes;) {
        if ((poll && !poll(context)) ||
            (reporter && reporter->ShouldAbortWork())) {
          stats.cancelled = true;
          ok = false;
          break;
        }
        const size_t n = std::min(size, bytes - offset);
        const ssize_t written = write(fd, buffer, n);
        ++stats.write_calls;
        if (written != (ssize_t)n) {
          ok = false;
          break;
        }
        stats.write_bytes += n;
        offset += n;
      }
      const u64 written = osGetTime();
      if (ok)
        ok = fsync(fd) == 0;
      const u64 committed = osGetTime();
      if (ok)
        ok = lseek(fd, 0, SEEK_SET) == 0;
      for (size_t offset = 0; ok && offset < bytes;) {
        if ((poll && !poll(context)) ||
            (reporter && reporter->ShouldAbortWork())) {
          stats.cancelled = true;
          ok = false;
          break;
        }
        const size_t n = std::min(size, bytes - offset);
        const ssize_t got = read(fd, buffer, n);
        ++stats.read_calls;
        if (got != (ssize_t)n) {
          ok = false;
          break;
        }
        for (size_t i = 0; i < n; ++i)
          if (buffer[i] != (unsigned char)(i * 73 + 29)) {
            ok = false;
            break;
          }
        stats.read_bytes += n;
        offset += n;
      }
      const u64 end = osGetTime();
      if (fd >= 0 && close(fd) != 0)
        ok = false;
      remove(path.c_str());
      DBG_LOGF(reporter,
               "CACHE IO probe block=%u bytes=%u reserve=%u reserve_ms=%llu "
               "write_ms=%llu commit_ms=%llu read_ms=%llu calls=%u/%u ok=%u",
               (unsigned)size, (unsigned)bytes, reserve ? 1 : 0,
               (unsigned long long)(reserved - begin),
               (unsigned long long)(written - reserved),
               (unsigned long long)(committed - written),
               (unsigned long long)(end - committed),
               (unsigned)(stats.write_calls - writes_before),
               (unsigned)(stats.read_calls - reads_before), ok ? 1 : 0);
      if (!ok) {
        delete[] buffer;
        return stats;
      }
    }
    delete[] buffer;
  }
  stats.ok = true;
  return stats;
}
} // namespace cache_io_probe

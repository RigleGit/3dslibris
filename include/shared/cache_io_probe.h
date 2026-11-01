#pragma once
#include <cstddef>
#include <string>
class IStatusReporter;
namespace cache_io_probe {
struct Stats {
  size_t read_bytes = 0, write_bytes = 0, read_calls = 0, write_calls = 0;
  bool ok = false, cancelled = false;
};
Stats Run(const std::string &temporary, IStatusReporter *reporter,
          bool (*poll)(void *), void *context, size_t bytes = 1024 * 1024);
} // namespace cache_io_probe

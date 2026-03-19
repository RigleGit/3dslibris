#pragma once

#include <functional>
#include <stddef.h>
#include <string>
#include <vector>

namespace buffered_status_log {

class BufferedStatusLog {
public:
  explicit BufferedStatusLog(size_t max_chunk_chars = 1024);

  void Append(const std::string &line);
  void Flush(const std::function<void(const std::string &)> &sink);

private:
  size_t max_chunk_chars_;
  std::vector<std::string> lines_;
};

} // namespace buffered_status_log

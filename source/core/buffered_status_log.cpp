#include "buffered_status_log.h"

#include <algorithm>

namespace buffered_status_log {

BufferedStatusLog::BufferedStatusLog(size_t max_chunk_chars)
    : max_chunk_chars_(std::max((size_t)1, max_chunk_chars)) {}

void BufferedStatusLog::Append(const std::string &line) {
  if (!line.empty())
    lines_.push_back(line);
}

void BufferedStatusLog::Flush(
    const std::function<void(const std::string &)> &sink) {
  if (!sink || lines_.empty()) {
    lines_.clear();
    return;
  }

  std::string chunk;
  for (size_t i = 0; i < lines_.size(); i++) {
    const std::string &line = lines_[i];
    if (line.empty())
      continue;

    const size_t extra = chunk.empty() ? line.size() : (size_t)1 + line.size();
    if (!chunk.empty() && chunk.size() + extra > max_chunk_chars_) {
      sink(chunk);
      chunk.clear();
    }

    if (!chunk.empty())
      chunk.push_back('\n');
    chunk += line;
  }

  if (!chunk.empty())
    sink(chunk);
  lines_.clear();
}

} // namespace buffered_status_log

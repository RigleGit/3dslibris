#pragma once

inline unsigned int NextBookSessionId(unsigned int previous_session_id) {
  unsigned int next_session_id = previous_session_id + 1;
  return next_session_id == 0 ? 1 : next_session_id;
}

inline bool ShouldCloseCurrentBookForSwitch(const void *current_book,
                                            const void *selected_book) {
  return current_book && current_book != selected_book;
}

inline bool ShouldAttachOpeningResult(unsigned int opening_session_id,
                                      unsigned int book_session_id,
                                      bool open_aborted, int page_count) {
  return opening_session_id != 0 && opening_session_id == book_session_id &&
         !open_aborted && page_count > 0;
}

inline const char *DescribeOpeningFailureCause(unsigned int opening_session_id,
                                               unsigned int book_session_id,
                                               bool open_aborted,
                                               int page_count) {
  if (opening_session_id == 0)
    return "stale-session";
  if (book_session_id != opening_session_id)
    return "session-mismatch";
  if (open_aborted)
    return "aborted";
  if (page_count <= 0)
    return "empty-parse";
  return "unknown";
}

#pragma once

inline unsigned int NextBookSessionId(unsigned int previous_session_id) {
  unsigned int next_session_id = previous_session_id + 1;
  return next_session_id == 0 ? 1 : next_session_id;
}

inline bool ShouldCloseCurrentBookForSwitch(const void *current_book,
                                            const void *selected_book) {
  return current_book && current_book != selected_book;
}

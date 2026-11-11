#pragma once
#include <cstddef>
#include <string>
class Book;

// Main-thread session; lifecycle hooks only set the signal flags.
class ReflowPreparation {
public:
  enum Result {
    Pending,
    Complete,
    Cancelled,
    Failed,
    NoMemory,
    IoError,
    Paused
  };
  ReflowPreparation();
  ~ReflowPreparation();
  bool Begin(Book *book, const std::string &source, size_t budget = 0,
             bool new3ds = false);
  Result Step();
  void Cancel();
  void SignalPause();
  void SignalResume();
  Result GetResult() const;
  const char *Label() const;
  unsigned Progress() const;
  unsigned Total() const;
  bool PersistenceFailed() const;
  // Debug accounting from the opening loop: call once per opening frame with
  // the frame timestamp, and report splash redraw cost separately.
  void NoteFrame(unsigned long long now_ms, unsigned work_ms);
  void NoteSplash(unsigned ms);
  // Time the opening loop may spend in Step() per frame (profile dependent).
  unsigned FrameBudgetMs() const;

private:
  struct State;
  State *state_;
  ReflowPreparation(const ReflowPreparation &);
  ReflowPreparation &operator=(const ReflowPreparation &);
};

#include "formats/common/reflow_preparation.h"
#include "formats/epub/epub_preparation.h"
#include <new>
#include <sys/stat.h>

struct ReflowPreparation::State {
  epub_parser::Preparation epub;
  std::string source;
  struct stat identity;
  bool paused = false, resume = false, source_error = false;
};
ReflowPreparation::ReflowPreparation() : state_(NULL) {}
ReflowPreparation::~ReflowPreparation() { delete state_; }
bool ReflowPreparation::Begin(Book *book, const std::string &source, size_t budget) {
  delete state_;
  state_ = new (std::nothrow) State;
  if (!state_) return false;
  state_->source = source;
  if (stat(source.c_str(), &state_->identity) != 0) return false;
  return state_->epub.Begin(book, source, budget);
}
void ReflowPreparation::SignalPause() { if (state_) state_->paused = true; }
void ReflowPreparation::SignalResume() { if (state_) state_->resume = true; }
ReflowPreparation::Result ReflowPreparation::Step() {
  if (!state_) return Failed;
  State &s = *state_;
  if (s.resume) {
    s.resume = false;
    struct stat identity;
    if (stat(s.source.c_str(), &identity) != 0 ||
        identity.st_size != s.identity.st_size || identity.st_mtime != s.identity.st_mtime)
      s.source_error = true;
    s.paused = false;
  }
  if (s.source_error) return IoError;
  if (s.paused) return Paused;
  return static_cast<Result>(s.epub.Step());
}
void ReflowPreparation::Cancel() { if (state_) state_->epub.Cancel(); }
ReflowPreparation::Result ReflowPreparation::GetResult() const {
  if (!state_) return Failed;
  if (state_->source_error) return IoError;
  return state_->paused ? Paused : static_cast<Result>(state_->epub.GetResult());
}
const char *ReflowPreparation::Label() const {
  if (!state_) return "cannot prepare book";
  return state_->source_error ? "source changed or unavailable" : state_->epub.Label();
}
unsigned ReflowPreparation::Progress() const { return state_ ? state_->epub.Progress() : 0; }
unsigned ReflowPreparation::Total() const { return state_ ? state_->epub.Total() : 0; }
bool ReflowPreparation::PersistenceFailed() const { return state_ && state_->epub.PersistenceFailed(); }

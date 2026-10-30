#pragma once
#include "library/library_walk.h"
namespace library_preload {
enum class Status {Idle,Running,Complete,Cancelled,WriteError};
enum class Phase {Scan,Metadata,Cover};
enum class Outcome {Cached,Saved,Absent,Unsupported,Error,WriteError,Cancelled};
struct Result {Outcome outcome;bool heavy;};
struct Counts {size_t cached=0,saved=0,absent=0,unsupported=0,errors=0;};
typedef Result (*Operation)(void *,const library_walk::Entry &);
class Engine {
public:
  bool Start(library_walk::RootSource,void *,Operation metadata,Operation cover);
  void Pump(unsigned entries=16,library_walk::Clock=nullptr,unsigned milliseconds=4);
  void Cancel();
  Status GetStatus() const {return status_;}
  Phase GetPhase() const {return phase_;}
  const library_walk::Entry &Current() const {return current_;}
  const Counts &MetadataCounts() const {return metadata_counts_;}
  const Counts &CoverCounts() const {return cover_counts_;}
  size_t Examined() const {return examined_;}
  size_t DirectoryErrors() const {return cursor_.Errors();}
private:
  library_walk::Cursor cursor_;
  library_walk::Entry current_;
  Status status_=Status::Idle;Phase phase_=Phase::Scan;
  Counts metadata_counts_,cover_counts_;size_t examined_=0;
  Operation metadata_=nullptr,cover_=nullptr;void *context_=nullptr;
  void Record(const Result &,Counts *);
};
}

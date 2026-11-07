#include "library/library_preload.h"
namespace library_preload {
bool Engine::Start(library_walk::RootSource roots,void *context,Operation metadata,Operation cover) {
  Cancel();metadata_counts_=Counts();cover_counts_=Counts();examined_=0;current_={};
  metadata_=metadata;cover_=cover;context_=context;phase_=Phase::Scan;
  if(!metadata || !cover || !cursor_.Start(roots,context,true)) {status_=Status::Idle;return false;}
  status_=Status::Running;return true;
}
void Engine::Record(const Result &result,Counts *counts) {
  switch(result.outcome) {
  case Outcome::Cached:++counts->cached;break;
  case Outcome::Saved:++counts->saved;break;
  case Outcome::Absent:++counts->absent;break;
  case Outcome::Unsupported:++counts->unsupported;break;
  case Outcome::Error:++counts->errors;break;
  case Outcome::WriteError:++counts->errors;status_=Status::WriteError;break;
  case Outcome::Cancelled:status_=Status::Cancelled;break;
  }
}
void Engine::Pump(unsigned entries,library_walk::Clock clock,unsigned milliseconds) {
  const uint64_t started=clock ? clock() : 0;unsigned visited=0;
  while(status_==Status::Running) {
    if(clock && clock()-started>=milliseconds) break;
    if(phase_==Phase::Scan) {
      if(visited++>=entries) break;
      const auto step=cursor_.Next(&current_);
      if(step==library_walk::Step::End) {status_=Status::Complete;break;}
      if(step==library_walk::Step::Skip) continue;
      if(!current_.directory) ++examined_;
      phase_=current_.directory ? Phase::Cover : Phase::Metadata;
    } else {
      const bool metadata=phase_==Phase::Metadata;
      const Result result=(metadata ? metadata_ : cover_)(context_,current_);
      Record(result,metadata ? &metadata_counts_ : &cover_counts_);
      phase_=metadata ? Phase::Cover : Phase::Scan;
      if(result.heavy) break;
    }
  }
  if(status_!=Status::Running) cursor_.Close();
}
void Engine::Cancel() {cursor_.Close();status_=Status::Cancelled;}
}

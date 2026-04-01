#pragma once

#include <cstddef>
#include <deque>

namespace browser_job_queue_utils {

inline bool IsHeavyBrowserJobType(int type, int metadata_job_type,
                                  int cover_job_type) {
  return type == metadata_job_type || type == cover_job_type;
}

template <typename JobT>
size_t PruneWarmupJobsForOtherBooks(std::deque<JobT> *jobs,
                                    const void *selected_book,
                                    int metadata_job_type,
                                    int cover_job_type) {
  if (!jobs)
    return 0;

  std::deque<JobT> kept;
  size_t removed = 0;
  while (!jobs->empty()) {
    JobT job = jobs->front();
    jobs->pop_front();
    const bool is_warmup =
        job.type == metadata_job_type || job.type == cover_job_type;
    if (is_warmup && job.book != selected_book) {
      removed++;
      continue;
    }
    kept.push_back(job);
  }
  jobs->swap(kept);
  return removed;
}

template <typename JobT, typename PredicateT>
bool TakeFirstAllowedJob(std::deque<JobT> *jobs, JobT *out,
                         PredicateT predicate) {
  if (!jobs || !out)
    return false;

  std::deque<JobT> kept;
  bool found = false;
  while (!jobs->empty()) {
    JobT job = jobs->front();
    jobs->pop_front();
    if (!found && predicate(job)) {
      *out = job;
      found = true;
      continue;
    }
    kept.push_back(job);
  }
  jobs->swap(kept);
  return found;
}

} // namespace browser_job_queue_utils

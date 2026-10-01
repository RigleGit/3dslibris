#include "app/library_controller.h"
#include "library/browser_job_queue_utils.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

namespace {

template <typename T, typename = void> struct IsComplete {
  static const bool value = false;
};
template <typename T> struct IsComplete<T, decltype(void(sizeof(T)))> {
  static const bool value = true;
};
static_assert(!IsComplete<App>::value,
              "LibraryController must compile without the App definition");

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, size_t actual, size_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void TestPrunesWarmupJobsForOtherBooks() {
  char a_storage = 0;
  Book *a = reinterpret_cast<Book *>(&a_storage);
  char b_storage = 0;
  Book *b = reinterpret_cast<Book *>(&b_storage);
  std::deque<app_job_t> jobs;
  jobs.push_back(app_job_t{APP_JOB_INDEX_METADATA, a});
  jobs.push_back(app_job_t{APP_JOB_EXTRACT_COVER, a});
  jobs.push_back(app_job_t{APP_JOB_INDEX_METADATA, b});
  jobs.push_back(app_job_t{APP_JOB_RESOLVE_TOC, b});

  const size_t removed = browser_job_queue_utils::PruneWarmupJobsForOtherBooks(
      &jobs, a, APP_JOB_INDEX_METADATA, APP_JOB_EXTRACT_COVER);
  ExpectEq("removed other warmup jobs", removed, (size_t)1);
  ExpectEq("remaining jobs", jobs.size(), (size_t)3);
  ExpectTrue("keeps selected warmup",
             jobs[0].book == a && jobs[0].type == APP_JOB_INDEX_METADATA);
  ExpectTrue("keeps selected cover",
             jobs[1].book == a && jobs[1].type == APP_JOB_EXTRACT_COVER);
  ExpectTrue("keeps non-warmup for others",
             jobs[2].book == b && jobs[2].type == APP_JOB_RESOLVE_TOC);
}

void TestNullSelectedPrunesAllWarmupJobs() {
  char a_storage = 0;
  Book *a = reinterpret_cast<Book *>(&a_storage);
  std::deque<app_job_t> jobs;
  jobs.push_back(app_job_t{APP_JOB_INDEX_METADATA, a});
  jobs.push_back(app_job_t{APP_JOB_EXTRACT_COVER, a});
  jobs.push_back(app_job_t{APP_JOB_RESOLVE_TOC, a});

  const size_t removed = browser_job_queue_utils::PruneWarmupJobsForOtherBooks(
      &jobs, NULL, APP_JOB_INDEX_METADATA, APP_JOB_EXTRACT_COVER);
  ExpectEq("removed all warmup jobs", removed, (size_t)2);
  ExpectEq("remaining non-warmup jobs", jobs.size(), (size_t)1);
  ExpectTrue("keeps non-warmup", jobs[0].type == APP_JOB_RESOLVE_TOC);
}

void TestHeavyBrowserJobClassification() {
  ExpectTrue("metadata is heavy",
             browser_job_queue_utils::IsHeavyBrowserJobType(
                 APP_JOB_INDEX_METADATA, APP_JOB_INDEX_METADATA,
                 APP_JOB_EXTRACT_COVER));
  ExpectTrue("cover is heavy",
             browser_job_queue_utils::IsHeavyBrowserJobType(
                 APP_JOB_EXTRACT_COVER, APP_JOB_INDEX_METADATA,
                 APP_JOB_EXTRACT_COVER));
  if (browser_job_queue_utils::IsHeavyBrowserJobType(
          APP_JOB_RESOLVE_TOC, APP_JOB_INDEX_METADATA, APP_JOB_EXTRACT_COVER))
    Fail("toc should not be treated as heavy browser job");
}

void TestTakeFirstAllowedJobPreservesOrderOfOthers() {
  char a_storage = 0;
  Book *a = reinterpret_cast<Book *>(&a_storage);
  std::deque<app_job_t> jobs;
  jobs.push_back(app_job_t{APP_JOB_INDEX_METADATA, a});
  jobs.push_back(app_job_t{APP_JOB_EXTRACT_COVER, a});
  jobs.push_back(app_job_t{APP_JOB_RESOLVE_TOC, a});

  app_job_t out = {};
  const bool found = browser_job_queue_utils::TakeFirstAllowedJob(
      &jobs, &out,
      [](const app_job_t &job) { return job.type == APP_JOB_EXTRACT_COVER; });
  ExpectTrue("finds allowed middle job", found);
  ExpectTrue("selected job is cover extraction", out.type == APP_JOB_EXTRACT_COVER);
  ExpectEq("remaining count after dequeue", jobs.size(), (size_t)2);
  ExpectTrue("preserves first remaining order",
             jobs[0].type == APP_JOB_INDEX_METADATA);
  ExpectTrue("preserves second remaining order",
             jobs[1].type == APP_JOB_RESOLVE_TOC);
}

} // namespace

int main() {
  TestPrunesWarmupJobsForOtherBooks();
  TestNullSelectedPrunesAllWarmupJobs();
  TestHeavyBrowserJobClassification();
  TestTakeFirstAllowedJobPreservesOrderOfOthers();
  return 0;
}

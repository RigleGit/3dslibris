#include "shared/browser_job_queue_utils.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

namespace {

struct FakeJob {
  int type;
  const void *book;
};

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
  int a = 1;
  int b = 2;
  std::deque<FakeJob> jobs;
  jobs.push_back(FakeJob{1, &a});
  jobs.push_back(FakeJob{2, &a});
  jobs.push_back(FakeJob{1, &b});
  jobs.push_back(FakeJob{3, &b});

  const size_t removed = browser_job_queue_utils::PruneWarmupJobsForOtherBooks(
      &jobs, &a, 1, 2);
  ExpectEq("removed other warmup jobs", removed, (size_t)1);
  ExpectEq("remaining jobs", jobs.size(), (size_t)3);
  ExpectTrue("keeps selected warmup", jobs[0].book == &a && jobs[0].type == 1);
  ExpectTrue("keeps selected toc", jobs[1].book == &a && jobs[1].type == 2);
  ExpectTrue("keeps non-warmup for others",
             jobs[2].book == &b && jobs[2].type == 3);
}

void TestNullSelectedPrunesAllWarmupJobs() {
  int a = 1;
  std::deque<FakeJob> jobs;
  jobs.push_back(FakeJob{1, &a});
  jobs.push_back(FakeJob{2, &a});
  jobs.push_back(FakeJob{3, &a});

  const size_t removed = browser_job_queue_utils::PruneWarmupJobsForOtherBooks(
      &jobs, NULL, 1, 2);
  ExpectEq("removed all warmup jobs", removed, (size_t)2);
  ExpectEq("remaining non-warmup jobs", jobs.size(), (size_t)1);
  ExpectTrue("keeps non-warmup", jobs[0].type == 3);
}

void TestHeavyBrowserJobClassification() {
  ExpectTrue("metadata is heavy",
             browser_job_queue_utils::IsHeavyBrowserJobType(1, 1, 2));
  ExpectTrue("cover is heavy",
             browser_job_queue_utils::IsHeavyBrowserJobType(2, 1, 2));
  if (browser_job_queue_utils::IsHeavyBrowserJobType(3, 1, 2))
    Fail("toc should not be treated as heavy browser job");
}

void TestTakeFirstAllowedJobPreservesOrderOfOthers() {
  int a = 1;
  std::deque<FakeJob> jobs;
  jobs.push_back(FakeJob{1, &a});
  jobs.push_back(FakeJob{2, &a});
  jobs.push_back(FakeJob{3, &a});

  FakeJob out = {0, NULL};
  const bool found = browser_job_queue_utils::TakeFirstAllowedJob(
      &jobs, &out, [](const FakeJob &job) { return job.type == 2; });
  ExpectTrue("finds allowed middle job", found);
  ExpectTrue("selected job is type 2", out.type == 2);
  ExpectEq("remaining count after dequeue", jobs.size(), (size_t)2);
  ExpectTrue("preserves first remaining order", jobs[0].type == 1);
  ExpectTrue("preserves second remaining order", jobs[1].type == 3);
}

} // namespace

int main() {
  TestPrunesWarmupJobsForOtherBooks();
  TestNullSelectedPrunesAllWarmupJobs();
  TestHeavyBrowserJobClassification();
  TestTakeFirstAllowedJobPreservesOrderOfOthers();
  return 0;
}

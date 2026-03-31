#include "shared/mobi_deferred_finalize_utils.h"

#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, mobi_deferred_finalize_utils::FinalizeStage actual,
              mobi_deferred_finalize_utils::FinalizeStage expected) {
  if (actual != expected)
    Fail(std::string(label) + ": unexpected finalize stage");
}

void TestFinalizeStageProgression() {
  using namespace mobi_deferred_finalize_utils;
  ExpectEq("not complete while stream incomplete",
           NextFinalizeStage(false, false, false, false, false, false),
           FinalizeStage::ContinuePaging);
  ExpectEq("metadata first",
           NextFinalizeStage(true, false, false, false, false, false),
           FinalizeStage::BuildMetadata);
  ExpectEq("structured toc second",
           NextFinalizeStage(true, true, false, false, false, false),
           FinalizeStage::LoadStructuredToc);
  ExpectEq("apply toc third",
           NextFinalizeStage(true, true, true, false, false, false),
           FinalizeStage::ApplyToc);
  ExpectEq("save cache fourth",
           NextFinalizeStage(true, true, true, true, false, false),
           FinalizeStage::SaveCache);
  ExpectEq("done last",
           NextFinalizeStage(true, true, true, true, true, true),
           FinalizeStage::Done);
}

} // namespace

int main() {
  TestFinalizeStageProgression();
  return 0;
}

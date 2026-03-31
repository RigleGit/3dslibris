#include "shared/deferred_relayout_utils.h"

#include <cstdlib>
#include <initializer_list>
#include <list>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void ExpectEqualInt(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectListEq(const char *label, const std::list<int> &actual,
                  std::initializer_list<int> expected) {
  std::list<int> expected_list(expected);
  if (actual != expected_list) {
    Fail(std::string(label) + ": unexpected bookmark remap");
  }
}

void TestPlanWithoutDeferredFinalRemap() {
  deferred_relayout_utils::OpenRelayoutPlan plan =
      deferred_relayout_utils::BuildOpenRelayoutPlan(true, false, 100, 50, 25,
                                                     std::list<int>{10, 50});
  ExpectFalse("no deferred remap when parse complete", plan.defer_final_remap);
  ExpectEqualInt("initial remap still applied", plan.mapped_position, 12);
  ExpectListEq("bookmarks remapped immediately", plan.mapped_bookmarks,
               {2, 12});
}

void TestPlanWithDeferredFinalRemap() {
  deferred_relayout_utils::OpenRelayoutPlan plan =
      deferred_relayout_utils::BuildOpenRelayoutPlan(true, true, 100, 50, 25,
                                                     std::list<int>{10, 50});
  ExpectTrue("deferred remap enabled", plan.defer_final_remap);
  ExpectEqualInt("initial remap uses partial page count", plan.mapped_position,
                 12);
  ExpectListEq("initial bookmarks use partial page count", plan.mapped_bookmarks,
               {2, 12});
}

void TestNoPlanWhenRelayoutNotNeeded() {
  deferred_relayout_utils::OpenRelayoutPlan plan =
      deferred_relayout_utils::BuildOpenRelayoutPlan(false, true, 100, 50, 25,
                                                     std::list<int>{10, 50});
  ExpectFalse("no remap pending", plan.has_remap);
  ExpectFalse("no final deferred remap", plan.defer_final_remap);
}

void TestFinalRemapAppliesOnlyIfUserStayedPut() {
  ExpectTrue("apply when parse done and position unchanged",
             deferred_relayout_utils::ShouldApplyFinalDeferredRelayout(
                 true, false, 12, 12));
  ExpectFalse("do not apply while parse still running",
              deferred_relayout_utils::ShouldApplyFinalDeferredRelayout(
                  true, true, 12, 12));
  ExpectFalse("do not apply after user moved",
              deferred_relayout_utils::ShouldApplyFinalDeferredRelayout(
                  true, false, 13, 12));
}

void TestFinalRemapCancellation() {
  ExpectTrue("cancel after user moved",
             deferred_relayout_utils::ShouldCancelFinalDeferredRelayout(
                 true, 13, 12));
  ExpectFalse("keep pending while user stays put",
              deferred_relayout_utils::ShouldCancelFinalDeferredRelayout(
                  true, 12, 12));
}

} // namespace

int main() {
  TestPlanWithoutDeferredFinalRemap();
  TestPlanWithDeferredFinalRemap();
  TestNoPlanWhenRelayoutNotNeeded();
  TestFinalRemapAppliesOnlyIfUserStayedPut();
  TestFinalRemapCancellation();
  return 0;
}

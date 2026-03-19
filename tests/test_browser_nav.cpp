#include "ui/browser_nav.h"

#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectState(const char *label, BrowserNavState actual, int selected,
                 int page_start) {
  if (actual.selected_index != selected || actual.page_start != page_start) {
    Fail(std::string(label) + ": unexpected browser navigation state");
  }
}

} // namespace

int main() {
  BrowserNavState state{0, 0};

  ExpectState("moves right inside page",
              BrowserNavMoveSelection(state, 8, 4, 2, BROWSER_NAV_RIGHT), 1,
              0);
  ExpectState("moves down by one row",
              BrowserNavMoveSelection(state, 8, 4, 2, BROWSER_NAV_DOWN), 2, 0);
  ExpectState("clamps at first item on left",
              BrowserNavMoveSelection(state, 8, 4, 2, BROWSER_NAV_LEFT), 0, 0);
  ExpectState("jumps to next page when moving past page boundary",
              BrowserNavMoveSelection({3, 0}, 8, 4, 2, BROWSER_NAV_RIGHT), 4,
              4);
  ExpectState("keeps same column when moving to next page row",
              BrowserNavMoveSelection({2, 0}, 8, 4, 2, BROWSER_NAV_DOWN), 4,
              4);
  ExpectState("clamps on short last page",
              BrowserNavMoveSelection({6, 4}, 7, 4, 2, BROWSER_NAV_RIGHT), 6,
              4);
  ExpectState("handles empty library",
              BrowserNavMoveSelection({5, 4}, 0, 4, 2, BROWSER_NAV_RIGHT), 0,
              0);

  return 0;
}

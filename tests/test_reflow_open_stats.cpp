#include "formats/common/reflow_open_stats.h"
#include "test_assert.h"
#include <cstring>

using namespace reflow_open_stats;

int main() {
  Stats s;
  test::ExpectEqU("fresh calls", s.calls[Merge], 0);
  char buf[256];
  s.FormatPhases(buf, sizeof(buf));
  test::ExpectEq("idle slots omitted", (int)strlen(buf), 0);

  s.Add(Read, 1500);
  s.Add(Read, 500);
  s.Add(Pages, 20000);
  s.NotePages(8);
  s.NotePages(3);
  test::ExpectEqU("calls", s.calls[Read], 2);
  test::ExpectEqU64("us", s.us[Read], 2000);
  test::ExpectEqU64("ms", s.Ms(Read), 2);
  test::ExpectEqU("max", s.max_us[Read], 1500);
  test::ExpectEqU("pages total", s.pages_total, 11);
  test::ExpectEqU("pages max", s.pages_max, 8);
  test::ExpectEqU("page calls", s.page_calls, 2);

  s.FormatPhases(buf, sizeof(buf));
  test::ExpectEq("format", strcmp(buf, "read=2/2 pages=20/1"), 0);

  // Truncation never overruns and keeps a terminator.
  char tiny[8];
  memset(tiny, 'x', sizeof(tiny));
  s.FormatPhases(tiny, sizeof(tiny));
  test::ExpectEq("tiny terminated", (int)strlen(tiny) < 8, 1);

  s.Add((Slot)99, 5); // out of range ignored
  s.Reset();
  test::ExpectEqU("reset", s.calls[Read], 0);
  test::ExpectEqU("reset pages", s.pages_total, 0);
  return 0;
}

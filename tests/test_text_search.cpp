#include "book/text_search.h"
#include "shared/text_token_constants.h"
#include <cassert>
#include <cstdio>
#include <vector>

struct Pages {
  std::vector<std::vector<uint32_t> > text;
  std::vector<page_search::Events> events;
  static bool Get(void *ctx, uint32_t i, text_search::PageView *view) {
    Pages &p = *(Pages *)ctx;
    if (i >= p.text.size()) return false;
    *view = {p.text[i].data(), p.text[i].size(), &p.events[i]};
    return true;
  }
  text_search::Source Source() { return {this, (uint32_t)text.size(), Get}; }
};
static text_search::Status Run(text_search::Engine &e, unsigned slice = 1) {
  unsigned calls = 0;
  while (e.GetStatus() == text_search::Searching && ++calls < 1000000) e.Pump(slice);
  assert(calls < 1000000);
  return e.GetStatus();
}
int main() {
  Pages p{{{'c','a','f',0xe9,' ', 'a',0xf1,'o',' ', 'a','n','o',' ',
            'b','a','n','a','n','a',' ',TEXT_LINK_START,99,'t','e',TEXT_LINK_END},
           {'s','t',' ', 'c','a','f','e',0x301}}, {{}, {}}};
  text_search::Engine e;
  assert(e.Configure("CAFÉ", false));
  e.Start(p.Source(), {}, false, true);
  assert(Run(e) == text_search::Found && e.GetMatch().start.token == 0);
  auto m = e.GetMatch();
  e.Start(p.Source(), m.start, false);
  assert(Run(e, 128) == text_search::Found && e.GetMatch().start.page == 1);
  e.Start(p.Source(), e.GetMatch().start, true);
  assert(Run(e) == text_search::Found && e.GetMatch().start.page == 0);
  assert(e.Configure("cafe", false));
  e.Start(p.Source(), {}, false, true);
  assert(Run(e) == text_search::NotFound); // Do not match half an accented grapheme.
  assert(e.Configure("ano", true));
  e.Start(p.Source(), {}, false, true);
  assert(Run(e) == text_search::Found && e.GetMatch().start.token == 9);
  assert(e.Configure("ana", false));
  e.Start(p.Source(), {}, false, true);
  assert(Run(e) == text_search::Found && e.GetMatch().start.token == 14);
  e.Start(p.Source(), e.GetMatch().start, false);
  assert(Run(e) == text_search::Found && e.GetMatch().start.token == 16);
  e.Start(p.Source(), e.GetMatch().start, false);
  assert(Run(e) == text_search::Found && e.Wrapped() && e.GetMatch().start.token == 14);
  assert(e.Configure("ana", true));
  e.Start(p.Source(), {}, false, true);
  assert(Run(e) == text_search::NotFound);
  assert(e.Configure("test", true));
  e.Start(p.Source(), {}, false, true);
  assert(Run(e) == text_search::Found && e.GetMatch().start.token == 22 && e.GetMatch().end.page == 1);
  e.Start(p.Source(), {1, 0}, true);
  assert(Run(e) == text_search::Found && e.GetMatch().start.page == 0);
  assert(!e.Configure("  \t", false));
  assert(!e.Configure("שלום", false));
  assert(!e.Configure(std::string(129, 'a'), false));
  assert(e.Configure("no result", false));
  e.Start(p.Source(), {}, false, true);
  e.Pump(3); e.Cancel();
  assert(e.GetStatus() == text_search::Idle);
  p = {{{'a','b','\n','c','d',' ',TEXT_FONT_SIZE,123,'a','b',TEXT_IMAGE,7,'c','d'}}, {{}}};
  page_search::Annotate(p.events[0], 2, page_search::Skip);
  assert(e.Configure("abcd", false));
  e.Start(p.Source(), {}, false, true);
  assert(Run(e) == text_search::Found && e.GetMatch().end.token == 5);
  e.Start(p.Source(), e.GetMatch().start, false);
  assert(Run(e) == text_search::NotFound);
  p = {{{'x', 0x5e9, 'a', 'b', 'c', ' ', 'o','k'}}, {{}}};
  page_search::Annotate(p.events[0], 2, page_search::RtlStart);
  page_search::Annotate(p.events[0], 5, page_search::RtlEnd);
  assert(e.Configure("abc", false));
  e.Start(p.Source(), {}, false, true);
  assert(Run(e) == text_search::NotFound && e.ExcludedRtl());
  assert(e.Configure("ok", false));
  e.Start(p.Source(), {}, true, true);
  assert(Run(e) == text_search::Found && e.Wrapped());
  p = {{{'a',0x315,0x300}}, {{}}};
  assert(e.Configure("à̕", true));
  e.Start(p.Source(), {}, false, true);
  assert(Run(e) == text_search::Found && e.GetMatch().end.token == 3);
  // Equivalent whitespace must not consume the cross-page lookahead budget.
  p = {{{'f','o','o'}, std::vector<uint32_t>(100, ' ')}, {{}, {}}};
  p.text[1].insert(p.text[1].end(), {'b','a','r'});
  assert(e.Configure("foo bar", true));
  e.Start(p.Source(), {}, false, true);
  assert(Run(e) == text_search::Found && e.GetMatch().end.page == 1);
  e.Start(p.Source(), {1, 103}, true);
  assert(Run(e) == text_search::Found && e.GetMatch().start.page == 0);
  p = {{{TEXT_LINK_START}}, {{}}};
  e.Start(p.Source(), {}, false, true);
  assert(Run(e) == text_search::Failed);
  p = {{std::vector<uint32_t>(20000, 'x')}, {{}}};
  assert(e.Configure("missing", false));
  e.Start(p.Source(), {}, false, true);
  const size_t memory = e.AuxiliaryBytes();
  e.Pump(10);
  assert(e.GetStatus() == text_search::Searching && e.Scanned() <= 10);
  assert(Run(e, 17) == text_search::NotFound && e.AuxiliaryBytes() == memory);
  std::puts("text search: Unicode, cross-page, overlap, wrap, whole words and cancellation passed");
}

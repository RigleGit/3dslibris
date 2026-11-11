#include "formats/mobi/mobi_position_map.h"
#include "test_assert.h"

#include <string>
#include <vector>

namespace {
unsigned g_seed = 31337;
unsigned Rand() {
  g_seed = g_seed * 1103515245u + 12345u;
  return (g_seed >> 8) & 0xffffff;
}

struct Cursor {
  const std::string *s;
  size_t last;
  bool bad;
};
unsigned char At(void *context, size_t index) {
  Cursor *c = static_cast<Cursor *>(context);
  if (index < c->last)
    c->bad = true; // the streamed remap promises forward-only access
  c->last = index;
  return (unsigned char)(*c->s)[index];
}
} // namespace

int main() {
  for (unsigned round = 0; round < 3000; ++round) {
    std::string before;
    const size_t n = Rand() % 900;
    for (size_t i = 0; i < n; ++i)
      before += (char)('a' + Rand() % (Rand() % 3 ? 4 : 26));
    std::string after;
    for (size_t i = 0; i < before.size(); ++i) {
      const unsigned r = Rand() % 12;
      if (r == 0)
        continue; // deleted
      after += r == 1 ? 'Z' : before[i];
      if (r == 2)
        after += "++";
    }
    std::vector<std::pair<uint32_t, uint32_t> > map;
    uint32_t html = 0, text = 0;
    map.push_back(std::make_pair(0u, 0u));
    const size_t entries = Rand() % 40;
    for (size_t e = 0; e < entries; ++e) {
      html += Rand() % 50;
      text += Rand() % 40;
      // Targets stay inside the text, as the extractor writes them.
      if (text > before.size())
        text = (uint32_t)before.size();
      map.push_back(std::make_pair(html, text));
    }
    std::vector<std::pair<uint32_t, uint32_t> > whole = map, streamed = map;
    mobi_position_map::RemapHtmlToTextAfterCleanup(before, after, &whole);
    Cursor cb = {&before, 0, false}, ca = {&after, 0, false};
    mobi_position_map::RemapHtmlToTextAfterCleanupStreamed(
        before.size(), after.size(), At, &cb, At, &ca, &streamed);
    test::ExpectTrue("same remap", whole == streamed);
    test::ExpectFalse("forward only before", cb.bad);
    test::ExpectFalse("forward only after", ca.bad);
  }
  return 0;
}

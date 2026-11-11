#include "formats/mobi/mobi_safe_markup_extract.h"
#include "test_assert.h"

#include <string>
#include <vector>

namespace {
unsigned g_seed = 777;
unsigned Rand() {
  g_seed = g_seed * 1103515245u + 12345u;
  return (g_seed >> 8) & 0xffffff;
}

struct Registry {
  std::vector<std::string> paths;
};
uint16_t Register(void *user, const std::string &path) {
  Registry *r = static_cast<Registry *>(user);
  r->paths.push_back(path);
  return (uint16_t)(r->paths.size() - 1);
}

std::string RandomMarkup(size_t size) {
  static const char *const pieces[] = {
      "<p>", "</p>", "<div class=\"x\">", "</div>", "<br/>", "<br />", "<li>",
      "</li>", "<h1>", "</h1>", "<script>", "</script>", "<style>", "</style>",
      "<img src=\"a.png\" recindex=\"0003\"/>", "<img recindex=\"7\">",
      "<a filepos=\"123\">", "</a>", "<b>", "<unknown attr='>'>", "&amp;",
      "&lt;", "&#13;", "&#x41;", "&#32;", "&nbsp;", "&foo;",
      "&aaaaaaaaaaaaaaaaa;", "&", "&;", "& ", " ", "  ", "\n", "\t", "\r\n",
      "\x01", "word", "text ", "caf\xc3\xa9", "\xe2\x82\xac", "<", ">", ";",
      "<tr>", "</tr>", "<section>", "<!-- c -->"};
  const size_t count = sizeof(pieces) / sizeof(pieces[0]);
  std::string out;
  while (out.size() < size)
    out += pieces[Rand() % count];
  return out;
}

typedef std::vector<std::pair<uint32_t, uint32_t> > Map;

void Check(const std::string &markup, bool with_map) {
  Registry expected_images, got_images;
  mobi_safe_markup_extract::InlineImageCallbacks a, b;
  a.register_inline_image = b.register_inline_image = Register;
  a.user_data = &expected_images;
  b.user_data = &got_images;

  Map expected_map, got_map;
  // The whole-buffer Session is the reference. Inputs ending inside a tag never
  // finish there, so strip an unterminated trailing '<' construct.
  std::string text = markup;
  for (size_t lt = text.rfind('<');
       lt != std::string::npos && text.find('>', lt) == std::string::npos;
       lt = text.rfind('<'))
    text.erase(lt);
  const std::string expected = mobi_safe_markup_extract::ExtractToText(
      text, a, with_map ? &expected_map : nullptr);

  mobi_safe_markup_extract::Stream stream;
  test::ExpectTrue("begin",
                   stream.Begin(b, with_map ? &got_map : nullptr, text.size()));
  std::string got, drained;
  size_t at = 0;
  const unsigned mode = Rand() % 3;
  while (at < text.size() || at == 0) {
    size_t step = mode == 0 ? 1 : mode == 1 ? 1 + Rand() % 9 : 1 + Rand() % 500;
    if (at + step > text.size())
      step = text.size() - at;
    const bool final = at + step == text.size();
    test::ExpectTrue("feed", stream.Feed(text.data() + at, step, final));
    stream.Drain(&got);
    at += step;
    if (final)
      break;
  }
  if (got != expected) {
    fprintf(stderr, "markup=[%s]\n", text.c_str());
    for (unsigned char ch : expected) fprintf(stderr, "%02x ", ch);
    fprintf(stderr, "\n");
    for (unsigned char ch : got) fprintf(stderr, "%02x ", ch);
    fprintf(stderr, "\n");
  }
  test::ExpectTrue("text equals ExtractToText", got == expected);
  test::ExpectEqU("output bytes", (unsigned)stream.OutputBytes(),
                  (unsigned)expected.size());
  test::ExpectTrue("map equals", got_map == expected_map);
  test::ExpectTrue("images equal", got_images.paths == expected_images.paths);
}
} // namespace

int main() {
  for (unsigned round = 0; round < 1500; ++round)
    Check(RandomMarkup(Rand() % 2500), Rand() % 2 == 0);
  // Long runs exercise the map sampling and the drain holdback.
  for (unsigned round = 0; round < 8; ++round)
    Check(RandomMarkup(300000), true);
  Check("", true);
  Check("<p>a</p>", true);
  Check("&amp", true);
  Check("a &", true);
  Check("<p>x</p>   \n  ", true);
  Check("   ", true);
  return 0;
}

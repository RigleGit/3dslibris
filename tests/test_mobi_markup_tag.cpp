#include "formats/mobi/mobi_markup_tag.h"

#include <cstdlib>
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

void ExpectEqInt(const char *label, int actual, int expected) {
  if (actual != expected)
    Fail(std::string(label) + ": unexpected integer value");
}

void ExpectEqSize(const char *label, size_t actual, size_t expected) {
  if (actual != expected)
    Fail(std::string(label) + ": unexpected size value");
}

} // namespace

int main() {
  MobiMarkupTagInfo info;

  ExpectTrue("img tag parses",
             mobi_parse_markup_tag("img recindex=\"12\" src=\"foo\"", &info));
  ExpectTrue("img tag valid", info.valid);
  ExpectFalse("img tag not closing", info.closing);
  ExpectEqInt("img kind", (int)info.kind, (int)MOBI_MARKUP_TAG_IMG);
  ExpectEqInt("img heading", info.heading_level, -1);
  ExpectEqSize("img attrs offset", info.attrs_offset, 3u);

  ExpectTrue("closing heading parses", mobi_parse_markup_tag(" /H2  ", &info));
  ExpectTrue("heading valid", info.valid);
  ExpectTrue("heading closing", info.closing);
  ExpectEqInt("heading kind", (int)info.kind, (int)MOBI_MARKUP_TAG_BLOCK);
  ExpectEqInt("heading level", info.heading_level, 1);

  ExpectTrue("pagebreak parses",
             mobi_parse_markup_tag(" mbp:pagebreak / ", &info));
  ExpectTrue("pagebreak valid", info.valid);
  ExpectEqInt("pagebreak kind", (int)info.kind, (int)MOBI_MARKUP_TAG_BLOCK);

  ExpectTrue("script parses", mobi_parse_markup_tag("script type='x'", &info));
  ExpectEqInt("script kind", (int)info.kind, (int)MOBI_MARKUP_TAG_SCRIPT);

  ExpectTrue("li parses", mobi_parse_markup_tag("li", &info));
  ExpectEqInt("li kind", (int)info.kind, (int)MOBI_MARKUP_TAG_LI);

  ExpectTrue("br parses", mobi_parse_markup_tag("Br/", &info));
  ExpectEqInt("br kind", (int)info.kind, (int)MOBI_MARKUP_TAG_BR);

  ExpectFalse("comment-like tag rejected", mobi_parse_markup_tag("   ", &info));
  return 0;
}

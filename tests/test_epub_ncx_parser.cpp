#include "formats/epub/epub_ncx_parser.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

static int g_pass = 0;
static int g_fail = 0;

[[noreturn]] void Fail(const char *label, const char *reason) {
  fprintf(stderr, "FAIL %s: %s\n", label, reason);
  g_fail++;
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(label, "expected true");
  g_pass++;
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(label, "expected false");
  g_pass++;
}

void ExpectEq(const char *label, size_t actual, size_t expected) {
  if (actual != expected) {
    char buf[128];
    snprintf(buf, sizeof(buf), "expected %zu, got %zu", expected, actual);
    Fail(label, buf);
  }
  g_pass++;
}

void ExpectStrEq(const char *label, const std::string &actual,
                 const char *expected) {
  if (actual != expected) {
    char buf[256];
    snprintf(buf, sizeof(buf), "expected '%s', got '%s'", expected,
             actual.c_str());
    Fail(label, buf);
  }
  g_pass++;
}

// ---- NCX (EPUB2) tests ----

void TestParseNcxBasic() {
  const char *xml = "<?xml version='1.0'?>"
                    "<ncx><navMap>"
                    "<navPoint><navLabel><text>Chapter 1</text></navLabel>"
                    "<content src='chapter1.html'/></navPoint>"
                    "<navPoint><navLabel><text>Chapter 2</text></navLabel>"
                    "<content src='chapter2.html'/></navPoint>"
                    "</navMap></ncx>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNcxWithExpat(xml, "", &entries);
  ExpectTrue("ncx basic: ok", ok);
  ExpectEq("ncx basic: count", entries.size(), 2);
  ExpectStrEq("ncx basic: title[0]", entries[0].title, "Chapter 1");
  ExpectStrEq("ncx basic: href[0]", entries[0].href, "chapter1.html");
  ExpectStrEq("ncx basic: title[1]", entries[1].title, "Chapter 2");
}

void TestParseNcxWithBasePath() {
  const char *xml = "<?xml version='1.0'?>"
                    "<ncx><navMap>"
                    "<navPoint><navLabel><text>Intro</text></navLabel>"
                    "<content src='text/intro.html'/></navPoint>"
                    "</navMap></ncx>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNcxWithExpat(xml, "OEBPS/toc.ncx", &entries);
  ExpectTrue("ncx basepath: ok", ok);
  ExpectEq("ncx basepath: count", entries.size(), 1);
  ExpectStrEq("ncx basepath: href", entries[0].href, "OEBPS/text/intro.html");
}

void TestParseNcxNestedLevels() {
  const char *xml = "<?xml version='1.0'?>"
                    "<ncx><navMap>"
                    "<navPoint>"
                    "  <navLabel><text>Part I</text></navLabel>"
                    "  <content src='part1.html'/>"
                    "  <navPoint>"
                    "    <navLabel><text>Section 1.1</text></navLabel>"
                    "    <content src='sec1_1.html'/>"
                    "  </navPoint>"
                    "</navPoint>"
                    "</navMap></ncx>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNcxWithExpat(xml, "", &entries);
  ExpectTrue("ncx nested: ok", ok);
  ExpectEq("ncx nested: count", entries.size(), 2);
  ExpectStrEq("ncx nested: title[0]", entries[0].title, "Section 1.1");
  ExpectStrEq("ncx nested: title[1]", entries[1].title, "Part I");
}

void TestParseNcxEmptyInput() {
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNcxWithExpat("", "", &entries);
  ExpectFalse("ncx empty: false", ok);
  ExpectEq("ncx empty: no entries", entries.size(), 0);
}

void TestParseNcxMalformed() {
  const char *xml = "<?xml version='1.0'?><ncx><navMap><navPoint>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNcxWithExpat(xml, "", &entries);
  ExpectFalse("ncx malformed: false", ok);
}

void TestParseNcxNullEntries() {
  const char *xml = "<?xml version='1.0'?><ncx><navMap>"
                    "<navPoint><navLabel><text>X</text></navLabel>"
                    "<content src='x.html'/></navPoint></navMap></ncx>";
  bool ok = epub_ncx_parser::ParseNcxWithExpat(xml, "", NULL);
  ExpectFalse("ncx null entries: false", ok);
}

void TestParseNcxWithFragmentHref() {
  const char *xml = "<?xml version='1.0'?>"
                    "<ncx><navMap>"
                    "<navPoint><navLabel><text>Anchor</text></navLabel>"
                    "<content src='chapter1.html#section2'/></navPoint>"
                    "</navMap></ncx>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNcxWithExpat(xml, "", &entries);
  ExpectTrue("ncx fragment: ok", ok);
  ExpectEq("ncx fragment: count", entries.size(), 1);
  ExpectStrEq("ncx fragment: href", entries[0].href, "chapter1.html#section2");
}

// ---- NCX lightweight fallback tests ----

void TestParseNcxLightweightBasic() {
  const char *xml = "<?xml version='1.0'?>"
                    "<ncx><navMap>"
                    "<navPoint><content src='ch1.html'/>"
                    "<navLabel><text>Chapter One</text></navLabel>"
                    "</navPoint>"
                    "</navMap></ncx>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNcxLightweight(xml, "", &entries);
  ExpectTrue("ncx lite: ok", ok);
  ExpectEq("ncx lite: count", entries.size(), 1);
  ExpectStrEq("ncx lite: title", entries[0].title, "Chapter One");
  ExpectStrEq("ncx lite: href", entries[0].href, "ch1.html");
}

void TestParseNcxLightweightEmpty() {
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNcxLightweight("", "", &entries);
  ExpectFalse("ncx lite empty: false", ok);
}

// ---- NAV (EPUB3) tests ----

void TestParseNavBasic() {
  const char *xml =
      "<?xml version='1.0'?>"
      "<html xmlns='http://www.w3.org/1999/xhtml'>"
      "<body>"
      "<nav epub:type='toc'>"
      "<ol>"
      "<li><a href='chapter1.html'>Chapter 1</a></li>"
      "<li><a href='chapter2.html'>Chapter 2</a></li>"
      "</ol>"
      "</nav>"
      "</body></html>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNavWithExpat(xml, "", &entries);
  ExpectTrue("nav basic: ok", ok);
  ExpectEq("nav basic: count", entries.size(), 2);
  ExpectStrEq("nav basic: title[0]", entries[0].title, "Chapter 1");
  ExpectStrEq("nav basic: href[0]", entries[0].href, "chapter1.html");
  ExpectStrEq("nav basic: title[1]", entries[1].title, "Chapter 2");
}

void TestParseNavWithBasePath() {
  const char *xml =
      "<?xml version='1.0'?>"
      "<html><body>"
      "<nav epub:type='toc'>"
      "<ol><li><a href='text/ch1.xhtml'>Ch 1</a></li></ol>"
      "</nav></body></html>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNavWithExpat(xml, "OEBPS/toc.xhtml", &entries);
  ExpectTrue("nav basepath: ok", ok);
  ExpectEq("nav basepath: count", entries.size(), 1);
  ExpectStrEq("nav basepath: href", entries[0].href, "OEBPS/text/ch1.xhtml");
}

void TestParseNavNestedLevels() {
  const char *xml =
      "<?xml version='1.0'?>"
      "<html><body>"
      "<nav epub:type='toc'><ol>"
      "<li><a href='part1.html'>Part I</a>"
      "  <ol>"
      "    <li><a href='ch1.html'>Chapter 1</a></li>"
      "  </ol>"
      "</li>"
      "</ol></nav></body></html>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNavWithExpat(xml, "", &entries);
  ExpectTrue("nav nested: ok", ok);
  ExpectEq("nav nested: count", entries.size(), 2);
  ExpectStrEq("nav nested: title[0]", entries[0].title, "Part I");
  ExpectStrEq("nav nested: title[1]", entries[1].title, "Chapter 1");
}

void TestParseNavEmpty() {
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNavWithExpat("", "", &entries);
  ExpectFalse("nav empty: false", ok);
  ExpectEq("nav empty: no entries", entries.size(), 0);
}

void TestParseNavMalformed() {
  const char *xml = "<?xml version='1.0'?><html><body><nav epub:type='toc'>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNavWithExpat(xml, "", &entries);
  ExpectFalse("nav malformed: false", ok);
}

void TestParseNavIgnoresNonTocNav() {
  const char *xml =
      "<?xml version='1.0'?>"
      "<html><body>"
      "<nav epub:type='landmarks'>"
      "<ol><li><a href='cover.html'>Cover</a></li></ol>"
      "</nav>"
      "<nav epub:type='toc'>"
      "<ol><li><a href='ch1.html'>Chapter 1</a></li></ol>"
      "</nav>"
      "</body></html>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNavWithExpat(xml, "", &entries);
  ExpectTrue("nav non-toc skipped: ok", ok);
  ExpectEq("nav non-toc skipped: only toc entries", entries.size(), 1);
  ExpectStrEq("nav non-toc skipped: href", entries[0].href, "ch1.html");
}

void TestParseNavWithFragment() {
  const char *xml =
      "<?xml version='1.0'?>"
      "<html><body>"
      "<nav epub:type='toc'>"
      "<ol><li><a href='ch1.html#intro'>Intro</a></li></ol>"
      "</nav></body></html>";
  std::vector<toc_entry_t> entries;
  bool ok = epub_ncx_parser::ParseNavWithExpat(xml, "", &entries);
  ExpectTrue("nav fragment: ok", ok);
  ExpectEq("nav fragment: count", entries.size(), 1);
  ExpectStrEq("nav fragment: href", entries[0].href, "ch1.html#intro");
}

} // namespace

int main() {
  TestParseNcxBasic();
  TestParseNcxWithBasePath();
  TestParseNcxNestedLevels();
  TestParseNcxEmptyInput();
  TestParseNcxMalformed();
  TestParseNcxNullEntries();
  TestParseNcxWithFragmentHref();
  TestParseNcxLightweightBasic();
  TestParseNcxLightweightEmpty();
  TestParseNavBasic();
  TestParseNavWithBasePath();
  TestParseNavNestedLevels();
  TestParseNavEmpty();
  TestParseNavMalformed();
  TestParseNavIgnoresNonTocNav();
  TestParseNavWithFragment();

  fprintf(stderr, "Results: %d/%d passed, %d failed\n", g_pass, g_pass + g_fail,
          g_fail);
  return g_fail > 0 ? 1 : 0;
}

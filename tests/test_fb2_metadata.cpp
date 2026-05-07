#include "formats/common/xml_parse_utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

static int g_pass = 0;
static int g_fail = 0;

[[noreturn]] void Fail(const char *label, const char *reason) {
  fprintf(stderr, "FAIL %s: %s\n", label, reason);
  g_fail++;
  std::exit(1);
}

void ExpectTrue(const char *label, bool v) {
  if (!v) Fail(label, "expected true");
  g_pass++;
}

void ExpectStrEq(const char *label, const std::string &actual, const char *expected) {
  if (actual != expected) {
    char buf[256];
    snprintf(buf, sizeof(buf), "expected '%s', got '%s'", expected, actual.c_str());
    Fail(label, buf);
  }
  g_pass++;
}

struct Fb2MetaState {
  std::string title;
  std::string first_name;
  std::string last_name;
  int depth;
  bool in_title_info;
  bool in_book_title;
  bool in_author;
  bool in_first_name;
  bool in_last_name;

  Fb2MetaState()
      : depth(0), in_title_info(false), in_book_title(false), in_author(false),
        in_first_name(false), in_last_name(false) {}
};

static const char *LocalName(const char *name) {
  const char *colon = strchr(name, ':');
  return colon ? colon + 1 : name;
}

static void fb2_start(void *user_data, const char *el, const char **) {
  Fb2MetaState *s = static_cast<Fb2MetaState *>(user_data);
  s->depth++;
  const char *ln = LocalName(el);
  if (!strcmp(ln, "title-info"))   s->in_title_info = true;
  if (s->in_title_info && !strcmp(ln, "book-title")) s->in_book_title = true;
  if (s->in_title_info && !strcmp(ln, "author"))     s->in_author = true;
  if (s->in_author && !strcmp(ln, "first-name"))     s->in_first_name = true;
  if (s->in_author && !strcmp(ln, "last-name"))      s->in_last_name = true;
}

static void fb2_char(void *user_data, const char *txt, int len) {
  Fb2MetaState *s = static_cast<Fb2MetaState *>(user_data);
  if (s->in_book_title)  s->title.append(txt, len);
  if (s->in_first_name)  s->first_name.append(txt, len);
  if (s->in_last_name)   s->last_name.append(txt, len);
}

static void fb2_end(void *user_data, const char *el) {
  Fb2MetaState *s = static_cast<Fb2MetaState *>(user_data);
  const char *ln = LocalName(el);
  if (!strcmp(ln, "book-title"))  s->in_book_title = false;
  if (!strcmp(ln, "first-name"))  s->in_first_name = false;
  if (!strcmp(ln, "last-name"))   s->in_last_name = false;
  if (!strcmp(ln, "author"))      s->in_author = false;
  if (!strcmp(ln, "title-info"))  s->in_title_info = false;
  s->depth--;
}

bool ParseFb2Meta(const std::string &xml, std::string *title_out,
                  std::string *author_out) {
  Fb2MetaState s;
  xml_parse_utils::XmlParserOptions opts;
  opts.start_element = fb2_start;
  opts.end_element   = fb2_end;
  opts.character_data = fb2_char;
  opts.user_data = &s;
  xml_parse_utils::XmlParseResult result =
      xml_parse_utils::ParseXmlString(xml, opts);
  if (!result.ok && s.title.empty())
    return false;
  if (title_out)
    *title_out = s.title;
  if (author_out) {
    *author_out = s.first_name;
    if (!s.first_name.empty() && !s.last_name.empty())
      author_out->push_back(' ');
    author_out->append(s.last_name);
  }
  return !s.title.empty();
}

// ---- tests ----

void TestBasicFb2Meta() {
  const char *xml =
      "<?xml version='1.0'?>"
      "<FictionBook xmlns='http://www.gribuser.ru/xml/fictionbook/2.0'>"
      "<description><title-info>"
      "<book-title>Basic FB2 Fixture</book-title>"
      "<author><first-name>Test</first-name><last-name>Author</last-name></author>"
      "</title-info></description>"
      "<body><section><p>Some text.</p></section></body>"
      "</FictionBook>";
  std::string title, author;
  ExpectTrue("basic fb2: parse ok", ParseFb2Meta(xml, &title, &author));
  ExpectStrEq("basic fb2: title", title, "Basic FB2 Fixture");
  ExpectStrEq("basic fb2: author", author, "Test Author");
}

void TestFb2FromFixtureFile() {
  const char *fixture_path =
      TEST_FIXTURES_DIR "/books/basic.fb2";
  FILE *fp = fopen(fixture_path, "r");
  if (!fp) {
    fprintf(stderr, "SKIP test_fb2_metadata: fixture not found: %s\n",
            fixture_path);
    return;
  }
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  std::string xml;
  xml.resize((size_t)sz);
  fread(&xml[0], 1, (size_t)sz, fp);
  fclose(fp);

  std::string title, author;
  ExpectTrue("fixture fb2: parse ok", ParseFb2Meta(xml, &title, &author));
  ExpectStrEq("fixture fb2: title", title, "Basic FB2 Fixture");
  ExpectStrEq("fixture fb2: author", author, "Test Author");
}

void TestFb2MissingTitle() {
  const char *xml =
      "<?xml version='1.0'?>"
      "<FictionBook><description><title-info>"
      "<author><first-name>Test</first-name></author>"
      "</title-info></description></FictionBook>";
  std::string title;
  bool ok = ParseFb2Meta(xml, &title, nullptr);
  if (ok)
    Fail("no title: should return false", "expected false return");
  g_pass++;
}

void TestFb2EmptyInput() {
  std::string title;
  bool ok = ParseFb2Meta("", &title, nullptr);
  if (ok)
    Fail("empty input: should return false", "expected false return");
  g_pass++;
}

void TestFb2NamespacedElements() {
  const char *xml =
      "<?xml version='1.0'?>"
      "<fb:FictionBook xmlns:fb='http://www.gribuser.ru/xml/fictionbook/2.0'>"
      "<fb:description><fb:title-info>"
      "<fb:book-title>Namespaced Title</fb:book-title>"
      "</fb:title-info></fb:description>"
      "</fb:FictionBook>";
  std::string title;
  ExpectTrue("namespaced fb2: parse ok", ParseFb2Meta(xml, &title, nullptr));
  ExpectStrEq("namespaced fb2: title", title, "Namespaced Title");
}

} // namespace

int main() {
  TestBasicFb2Meta();
  TestFb2FromFixtureFile();
  TestFb2MissingTitle();
  TestFb2EmptyInput();
  TestFb2NamespacedElements();

  fprintf(stderr, "Results: %d/%d passed, %d failed\n", g_pass, g_pass + g_fail,
          g_fail);
  return g_fail > 0 ? 1 : 0;
}

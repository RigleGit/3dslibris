#include "formats/mobi/mobi_safe_markup_extract.h"
#include "shared/text_token_constants.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &msg) {
  std::fprintf(stderr, "%s\n", msg.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, const std::string &actual,
              const std::string &expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected [" + expected + "], got [" + actual +
         "]");
  }
}

struct FakeInlineImageRegistry {
  std::vector<std::string> paths;
};

uint16_t RegisterInlineImage(void *user_data, const std::string &path) {
  FakeInlineImageRegistry *registry =
      static_cast<FakeInlineImageRegistry *>(user_data);
  registry->paths.push_back(path);
  return (uint16_t)(registry->paths.size() - 1);
}

void TestParagraphsAndEntities() {
  const std::string html =
      "<html><body><p>Hello &amp; welcome</p><p>World</p></body></html>";
  ExpectEq("paragraphs", mobi_safe_markup_extract::ExtractToText(html),
           "Hello & welcome\n\nWorld");
}

void TestScriptAndStyleAreIgnored() {
  const std::string html =
      "<style>.x{color:red}</style><script>alert('x')</script><p>Visible</p>";
  ExpectEq("script/style", mobi_safe_markup_extract::ExtractToText(html),
           "Visible");
}

void TestListItemsGetSimpleBullets() {
  const std::string html = "<ul><li>One</li><li>Two</li></ul>";
  ExpectEq("list items", mobi_safe_markup_extract::ExtractToText(html),
           "- One\n\n- Two");
}

void TestInlineImagesBecomeTokens() {
  FakeInlineImageRegistry registry;
  mobi_safe_markup_extract::InlineImageCallbacks callbacks;
  callbacks.register_inline_image = RegisterInlineImage;
  callbacks.user_data = &registry;

  const std::string html = "<p>Before</p><p><img recindex=\"42\"/>After</p>";
  const std::string actual =
      mobi_safe_markup_extract::ExtractToText(html, callbacks);

  if (registry.paths.size() != 1 || registry.paths[0] != "mobi:img:42") {
    Fail("inline image path registration failed");
  }
  if (actual.size() < 4) {
    Fail("inline image token output too short");
  }
  const size_t token_pos = actual.find((char)TEXT_IMAGE_LEADING_PARAGRAPH);
  if (token_pos == std::string::npos || token_pos + 3 >= actual.size() ||
      (unsigned char)actual[token_pos + 1] != TEXT_IMAGE ||
      (unsigned char)actual[token_pos + 2] != 0 ||
      (unsigned char)actual[token_pos + 3] != 0) {
    Fail("inline image token sequence mismatch");
  }
}

} // namespace

int main() {
  TestParagraphsAndEntities();
  TestScriptAndStyleAreIgnored();
  TestListItemsGetSimpleBullets();
  TestInlineImagesBecomeTokens();
  return 0;
}

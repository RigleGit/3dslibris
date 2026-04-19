#include "formats/common/text_helpers.h"
#include "test_assert.h"

#include <string>

namespace {

void TestNormalizeNewlinesNoOpWithoutCarriageReturns() {
  std::string text = "line one\nline two\nline three";
  NormalizeNewlines(&text);
  test::ExpectStrEq("newline no-op", text.c_str(),
                    "line one\nline two\nline three");
}

void TestNormalizeNewlinesCollapsesCrLfAndCr() {
  std::string text = "a\r\nb\rc";
  NormalizeNewlines(&text);
  test::ExpectStrEq("newline normalize", text.c_str(), "a\nb\nc");
}

} // namespace

int main() {
  TestNormalizeNewlinesNoOpWithoutCarriageReturns();
  TestNormalizeNewlinesCollapsesCrLfAndCr();
  return 0;
}

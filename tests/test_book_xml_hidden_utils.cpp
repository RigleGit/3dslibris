#include "book/book_xml_hidden_utils.h"
#include "test_assert.h"

namespace {

void TestRecognizesCosmeticPageBreak() {
  const char *attrs[] = {"aria-hidden", "true", "epub:type", "pagebreak",
                         "id", "pg_x", "role", "doc-pagebreak", NULL};
  test::ExpectTrue("cosmetic pagebreak",
                   book_xml_hidden_utils::IsCosmeticPageBreakElement(attrs));
}

void TestRequiresHiddenFlag() {
  const char *attrs[] = {"epub:type", "pagebreak", "role", "doc-pagebreak",
                         NULL};
  test::ExpectFalse("visible pagebreak not cosmetic",
                    book_xml_hidden_utils::IsCosmeticPageBreakElement(attrs));
}

void TestIgnoresUnrelatedHiddenSpan() {
  const char *attrs[] = {"aria-hidden", "true", "id", "foo", NULL};
  test::ExpectFalse("hidden non-pagebreak span",
                    book_xml_hidden_utils::IsCosmeticPageBreakElement(attrs));
}

void TestRecognizesTokenizedType() {
  const char *attrs[] = {"aria-hidden", "TRUE", "epub:type",
                         "pagebreak footnote", NULL};
  test::ExpectTrue("pagebreak token among others",
                   book_xml_hidden_utils::IsCosmeticPageBreakElement(attrs));
}

} // namespace

int main() {
  TestRecognizesCosmeticPageBreak();
  TestRequiresHiddenFlag();
  TestIgnoresUnrelatedHiddenSpan();
  TestRecognizesTokenizedType();
  return 0;
}

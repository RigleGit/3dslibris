#include "formats/mobi/mobi_toc_finalize_policy.h"
#include "test_assert.h"

#include <utility>
#include <vector>

namespace {

void TestSkipsStructuredTocWithoutHtmlMap() {
  std::vector<std::pair<uint32_t, uint32_t>> html_to_text_map;
  std::vector<uint32_t> text_cursor_per_page;
  text_cursor_per_page.push_back(0);
  text_cursor_per_page.push_back(128);

  test::ExpectFalse("skip structured toc without map",
                    mobi_toc_finalize_policy::ShouldApplyStructuredToc(
                        html_to_text_map, text_cursor_per_page));
}

void TestSkipsStructuredTocWhenMapEndsBeforePages() {
  std::vector<std::pair<uint32_t, uint32_t>> html_to_text_map;
  html_to_text_map.push_back(std::make_pair(0u, 0u));
  html_to_text_map.push_back(std::make_pair(1024u, 64u));

  std::vector<uint32_t> text_cursor_per_page;
  text_cursor_per_page.push_back(0);
  text_cursor_per_page.push_back(512);

  test::ExpectFalse("skip structured toc with truncated map",
                    mobi_toc_finalize_policy::ShouldApplyStructuredToc(
                        html_to_text_map, text_cursor_per_page));
}

void TestAppliesStructuredTocWhenMapLooksUsable() {
  std::vector<std::pair<uint32_t, uint32_t>> html_to_text_map;
  html_to_text_map.push_back(std::make_pair(0u, 0u));
  html_to_text_map.push_back(std::make_pair(1024u, 2048u));

  std::vector<uint32_t> text_cursor_per_page;
  text_cursor_per_page.push_back(0);
  text_cursor_per_page.push_back(512);
  text_cursor_per_page.push_back(1536);

  test::ExpectTrue("apply structured toc with usable map",
                   mobi_toc_finalize_policy::ShouldApplyStructuredToc(
                       html_to_text_map, text_cursor_per_page));
}

} // namespace

int main() {
  TestSkipsStructuredTocWithoutHtmlMap();
  TestSkipsStructuredTocWhenMapEndsBeforePages();
  TestAppliesStructuredTocWhenMapLooksUsable();
  return 0;
}

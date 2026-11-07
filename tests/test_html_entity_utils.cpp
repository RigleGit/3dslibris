#include "allocation_counter.h"
#include "formats/common/html_entity_utils.h"
#include "test_assert.h"

#include <string>

namespace {

void TestDecodesPortugueseNamedEntities() {
  uint32_t cp = 0;
  test::ExpectTrue("atilde decodes",
                   html_entity_utils::DecodeHtmlEntityCodepoint("atilde", &cp));
  test::ExpectEq("atilde codepoint", (int)cp, 0x00E3);

  test::ExpectTrue("uacute decodes",
                   html_entity_utils::DecodeHtmlEntityCodepoint("&uacute;", &cp));
  test::ExpectEq("uacute codepoint", (int)cp, 0x00FA);

  test::ExpectTrue("agrave decodes",
                   html_entity_utils::DecodeHtmlEntityCodepoint("agrave", &cp));
  test::ExpectEq("agrave codepoint", (int)cp, 0x00E0);

  test::ExpectTrue("ocirc decodes",
                   html_entity_utils::DecodeHtmlEntityCodepoint("ocirc", &cp));
  test::ExpectEq("ocirc codepoint", (int)cp, 0x00F4);
}

void TestDecodesPunctuationEntities() {
  uint32_t cp = 0;
  test::ExpectTrue("ndash decodes",
                   html_entity_utils::DecodeHtmlEntityCodepoint("ndash", &cp));
  test::ExpectEq("ndash codepoint", (int)cp, 0x2013);

  test::ExpectTrue("hellip decodes",
                   html_entity_utils::DecodeHtmlEntityCodepoint("hellip", &cp));
  test::ExpectEq("hellip codepoint", (int)cp, 0x2026);
}

void TestDecodesNumericEntities() {
  uint32_t cp = 0;
  test::ExpectTrue("decimal entity",
                   html_entity_utils::DecodeHtmlEntityCodepoint("&#227;", &cp));
  test::ExpectEq("decimal codepoint", (int)cp, 0x00E3);

  test::ExpectTrue("hex entity",
                   html_entity_utils::DecodeHtmlEntityCodepoint("&#x2013;", &cp));
  test::ExpectEq("hex codepoint", (int)cp, 0x2013);
}

void TestEncodesUtf8Output() {
  std::string out;
  test::ExpectTrue("utf8 entity output",
                   html_entity_utils::DecodeHtmlEntityUtf8("ccedil", &out));
  test::ExpectStrEq("utf8 ccedil", out.c_str(), "\xC3\xA7");
}

void TestRejectsInvalidEntities() {
  uint32_t cp = 0;
  test::ExpectFalse("unknown entity",
                    html_entity_utils::DecodeHtmlEntityCodepoint("nope", &cp));
  test::ExpectFalse("invalid scalar",
                    html_entity_utils::DecodeHtmlEntityCodepoint("&#0;", &cp));
  test::ExpectFalse(
      "surrogate scalar",
      html_entity_utils::DecodeHtmlEntityCodepoint("&#xD800;", &cp));
}

void TestNormalizesNamedEntitiesForXml() {
  const std::string normalized = html_entity_utils::NormalizeHtmlNamedEntitiesForXml(
      "Portugu&ecirc;s &amp; cora&ccedil;&atilde;o &ndash; &#227;");
  test::ExpectStrEq("named entities normalized", normalized.c_str(),
                    "Portugu&#234;s &amp; cora&#231;&#227;o &#8211; &#227;");
}

void TestNormalizesNamedEntitiesAcrossChunks() {
  html_entity_utils::ChunkedEntityNormalizerState state;
  std::string out;
  html_entity_utils::NormalizeHtmlNamedEntitiesForXmlChunk(
      "&ec", 3, false, &state, &out);
  html_entity_utils::NormalizeHtmlNamedEntitiesForXmlChunk(
      "irc; e &nd", 10, false, &state, &out);
  html_entity_utils::NormalizeHtmlNamedEntitiesForXmlChunk(
      "ash;", 4, true, &state, &out);
  test::ExpectStrEq("chunked entities normalized", out.c_str(),
                    "&#234; e &#8211;");
}

void TestEveryChunkBoundary() {
  const std::string inputs[] = {"plain &nbsp; A&amp;B &ecirc; &unknown; tail&",
      "&" + std::string(28, 'x') + "; &ndA &ndash;",
      std::string("a\0b &ecirc;", 12)};
  for (const auto &input : inputs) {
    const auto expected = html_entity_utils::NormalizeHtmlNamedEntitiesForXml(input);
    for (size_t a = 0; a <= input.size(); ++a)
      for (size_t b = a; b <= input.size(); ++b) {
        html_entity_utils::ChunkedEntityNormalizerState state;
        std::string out;
        html_entity_utils::NormalizeHtmlNamedEntitiesForXmlChunk(input.data(), a, false, &state, &out);
        html_entity_utils::NormalizeHtmlNamedEntitiesForXmlChunk(input.data() + a, b-a, false, &state, &out);
        html_entity_utils::NormalizeHtmlNamedEntitiesForXmlChunk(input.data() + b, input.size()-b, true, &state, &out);
        test::ExpectTrue("all chunk boundaries preserve whole-input normalization", out == expected);
      }
  }
  const std::string plain(16384, 'a');
  html_entity_utils::ChunkedEntityNormalizerState state;
  std::string out; out.reserve(plain.size());
  allocation_count = 0; count_allocations = true;
  html_entity_utils::NormalizeHtmlNamedEntitiesForXmlChunk(plain.data(), plain.size(), true, &state, &out);
  count_allocations = false;
  test::ExpectTrue("plain chunk bytes preserved", out == plain);
  test::ExpectEq("plain chunks do not allocate an input copy", (int)allocation_count, 0);
}

} // namespace

int main() {
  TestEveryChunkBoundary();
  TestDecodesPortugueseNamedEntities();
  TestDecodesPunctuationEntities();
  TestDecodesNumericEntities();
  TestEncodesUtf8Output();
  TestRejectsInvalidEntities();
  TestNormalizesNamedEntitiesForXml();
  TestNormalizesNamedEntitiesAcrossChunks();
  return 0;
}

#include "formats/mobi/mobi_decode_plan.h"

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

void TestSmallMobiKeepsImmediateMetadata() {
  const mobi_decode_plan::Plan plan =
      mobi_decode_plan::Build(256 * 1024);
  ExpectFalse("small defer_toc_finalize", plan.defer_toc_finalize);
  ExpectTrue("small capture_toc_metadata", plan.capture_toc_metadata);
  ExpectFalse("small retain_markup_utf8", plan.retain_markup_utf8);
}

void TestLargeMobiDefersFinalizeButKeepsMetadata() {
  const mobi_decode_plan::Plan plan =
      mobi_decode_plan::Build(2 * 1024 * 1024);
  ExpectTrue("large defer_toc_finalize", plan.defer_toc_finalize);
  ExpectTrue("large capture_toc_metadata", plan.capture_toc_metadata);
  ExpectFalse("large retain_markup_utf8", plan.retain_markup_utf8);
}

} // namespace

int main() {
  TestSmallMobiKeepsImmediateMetadata();
  TestLargeMobiDefersFinalizeButKeepsMetadata();
  return 0;
}

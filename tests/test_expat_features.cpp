#include "expat.h"

#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

bool HasFeature(enum XML_FeatureEnum feature, long *value_out) {
  const XML_Feature *features = XML_GetFeatureList();
  if (!features)
    return false;
  for (const XML_Feature *it = features; it->feature != XML_FEATURE_END; ++it) {
    if (it->feature == feature) {
      if (value_out)
        *value_out = it->value;
      return true;
    }
  }
  return false;
}

} // namespace

int main() {
  const XML_Expat_Version version = XML_ExpatVersionInfo();
  ExpectEq("expat major", version.major, 2);
  ExpectEq("expat minor", version.minor, 7);
  ExpectEq("expat micro", version.micro, 5);

  long context_bytes = -1;
  const bool has_dtd = HasFeature(XML_FEATURE_DTD, NULL);
  const bool has_ns = HasFeature(XML_FEATURE_NS, NULL);
  const bool has_context = HasFeature(XML_FEATURE_CONTEXT_BYTES, &context_bytes);

  ExpectEq("expected dtd", has_dtd ? 1 : 0, EXPECT_DTD);
  ExpectEq("expected ns", has_ns ? 1 : 0, EXPECT_NS);
  if (EXPECT_CONTEXT_BYTES == 0) {
    ExpectEq("context feature absent at zero", has_context ? 1 : 0, 0);
  } else {
    ExpectTrue("context feature present when enabled", has_context);
    ExpectEq("context bytes", (int)context_bytes, EXPECT_CONTEXT_BYTES);
  }

  return 0;
}

#include "formats/common/xml_parse_utils.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

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

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectSizeLt(const char *label, size_t actual, size_t limit) {
  if (!(actual < limit)) {
    Fail(std::string(label) + ": expected less than " +
         std::to_string(limit));
  }
}

void ExpectNonZero(const char *label, int actual) {
  if (actual == 0)
    Fail(std::string(label) + ": expected non-zero");
}

struct ScanState {
  int start_count;
  int end_count;
  int item_count;
  bool stop;
  std::string text;
};

void ScanStart(void *user_data, const char *el, const char **) {
  ScanState *state = static_cast<ScanState *>(user_data);
  if (!state)
    return;
  state->start_count++;
  if (el && !strcmp(el, "item"))
    state->item_count++;
}

void ScanEnd(void *user_data, const char *) {
  ScanState *state = static_cast<ScanState *>(user_data);
  if (!state)
    return;
  state->end_count++;
}

void ScanChar(void *user_data, const char *txt, int txtlen) {
  ScanState *state = static_cast<ScanState *>(user_data);
  if (!state || !txt || txtlen <= 0)
    return;
  state->text.append(txt, txtlen);
  if (state->text == "1")
    state->stop = true;
}

bool ShouldStop(void *user_data) {
  ScanState *state = static_cast<ScanState *>(user_data);
  return state && state->stop;
}

struct UnknownEncodingState {
  int calls;
};

int RejectUnknownEncoding(void *data, const XML_Char *, XML_Encoding *) {
  UnknownEncodingState *state = static_cast<UnknownEncodingState *>(data);
  if (state)
    state->calls++;
  return XML_STATUS_ERROR;
}

std::string WriteTempFile(const char *contents) {
  char path[] = "/tmp/3dslibris-xml-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    Fail("mkstemp failed");
  size_t len = strlen(contents);
  if (write(fd, contents, len) != (ssize_t)len) {
    close(fd);
    unlink(path);
    Fail("write failed");
  }
  close(fd);
  return std::string(path);
}

} // namespace

int main() {
  using xml_parse_utils::ParseXmlBuffer;
  using xml_parse_utils::ParseXmlFileStream;

  {
    ScanState state = {0, 0, 0, false, ""};
    xml_parse_utils::XmlParserOptions options;
    options.start_element = ScanStart;
    options.end_element = ScanEnd;
    options.character_data = ScanChar;
    options.user_data = &state;
    const char *xml = "<root><item>hello</item><item>world</item></root>";
    xml_parse_utils::XmlParseResult result =
        ParseXmlBuffer(xml, strlen(xml), options);
    ExpectTrue("buffer parse ok", result.ok);
    ExpectEq("buffer item count", state.item_count, 2);
    ExpectEq("buffer start count", state.start_count, 3);
    ExpectEq("buffer end count", state.end_count, 3);
    ExpectTrue("buffer text", state.text == "helloworld");
  }

  {
    std::string path = WriteTempFile("<root><item>stream</item></root>");
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
      Fail("fopen failed");

    ScanState state = {0, 0, 0, false, ""};
    xml_parse_utils::XmlParserOptions options;
    options.start_element = ScanStart;
    options.end_element = ScanEnd;
    options.character_data = ScanChar;
    options.user_data = &state;
    xml_parse_utils::XmlParseResult result =
        ParseXmlFileStream(fp, options, 7);
    fclose(fp);
    unlink(path.c_str());

    ExpectTrue("file parse ok", result.ok);
    ExpectEq("file item count", state.item_count, 1);
    ExpectTrue("file text", state.text == "stream");
  }

  {
    ScanState state = {0, 0, 0, false, ""};
    xml_parse_utils::XmlParserOptions options;
    options.start_element = ScanStart;
    options.end_element = ScanEnd;
    options.character_data = ScanChar;
    options.user_data = &state;
    const char *xml = "<root><item></root>";
    xml_parse_utils::XmlParseResult result =
        ParseXmlBuffer(xml, strlen(xml), options);
    ExpectFalse("syntax error reported", result.ok);
    ExpectEq("syntax error code", (int)result.error_code,
             (int)XML_ERROR_TAG_MISMATCH);
    ExpectNonZero("syntax line", result.error_line);
    ExpectNonZero("syntax column", result.error_column);
  }

  {
    std::string path = WriteTempFile("<root><item>1</item><item>2</item></root>");
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
      Fail("fopen failed");

    ScanState state = {0, 0, 0, false, ""};
    xml_parse_utils::XmlParserOptions options;
    options.start_element = ScanStart;
    options.end_element = ScanEnd;
    options.character_data = ScanChar;
    options.user_data = &state;
    xml_parse_utils::XmlParseResult result =
        ParseXmlFileStream(fp, options, 1, ShouldStop);
    fclose(fp);
    unlink(path.c_str());

    ExpectTrue("early stop still ok", result.ok);
    ExpectEq("early stop item count", state.item_count, 1);
    ExpectTrue("early stop text", state.text == "1");
    ExpectSizeLt("early stop consumed bytes", result.bytes_consumed,
                 strlen("<root><item>1</item><item>2</item></root>"));
  }

  {
    UnknownEncodingState encoding_state = {0};
    xml_parse_utils::XmlParserOptions options;
    options.unknown_encoding = RejectUnknownEncoding;
    options.unknown_encoding_data = &encoding_state;
    const char *xml = "<?xml version=\"1.0\" encoding=\"x-test\"?><root/>";
    xml_parse_utils::XmlParseResult result =
        ParseXmlBuffer(xml, strlen(xml), options);
    ExpectFalse("unknown encoding fails", result.ok);
    ExpectEq("unknown encoding callback count", encoding_state.calls, 1);
    ExpectEq("unknown encoding error", (int)result.error_code,
             (int)XML_ERROR_UNKNOWN_ENCODING);
  }

  return 0;
}

#include "heading_layout.h"

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

heading_layout::KeepWithNextRequest BaseRequest() {
  heading_layout::KeepWithNextRequest req{};
  req.pen_y = 26;
  req.screen_height = 400;
  req.bottom_margin = 36;
  req.line_height = 16;
  req.linespacing = 1;
  req.heading_level = 1;
  return req;
}

} // namespace

int main() {
  using heading_layout::ShouldAdvanceHeadingForKeepWithNext;

  {
    heading_layout::KeepWithNextRequest req = BaseRequest();
    req.pen_y = 300;
    ExpectFalse("h1 stays when heading plus following lines fit",
                ShouldAdvanceHeadingForKeepWithNext(req));
  }

  {
    heading_layout::KeepWithNextRequest req = BaseRequest();
    req.pen_y = 332;
    ExpectTrue("h1 advances when only orphan heading would fit",
               ShouldAdvanceHeadingForKeepWithNext(req));
  }

  {
    heading_layout::KeepWithNextRequest req = BaseRequest();
    req.heading_level = 2;
    req.pen_y = 349;
    ExpectTrue("h2 also advances near bottom", 
               ShouldAdvanceHeadingForKeepWithNext(req));
  }

  {
    heading_layout::KeepWithNextRequest req = BaseRequest();
    req.heading_level = 3;
    req.pen_y = 331;
    ExpectFalse("h3 is allowed when minimum keep-with-next block fits",
                ShouldAdvanceHeadingForKeepWithNext(req));
  }

  return 0;
}

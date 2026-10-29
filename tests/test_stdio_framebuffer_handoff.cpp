#include "ui/text.h"
#include "ui/font_constants.h"
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Platform boundary: consoleInit attaches both streams to a persistent RGB565
// framebuffer. Switching gfx to BGR8 does not detach those stream devices.
static std::vector<u8> bottom(240u * 320u * 3u);
static std::string debug_output;
static void ConsoleWrite(const char *, size_t size) {
  // One 16-bit console pixel is enough to detect an out-of-band write. The
  // SDK also scrolls this same allocation using its old RGB565 geometry.
  if (size) bottom[0] = bottom[1] = 0;
}
static void DebugWrite(const char *data, size_t size) {
  debug_output.append(data, size);
}
struct Device { void (*write)(const char *, size_t); };
static const Device console_device = {ConsoleWrite};
static const Device debug_device = {DebugWrite};
enum { STD_OUT = 1, STD_ERR = 2 };
static const Device *devoptab_list[] = {nullptr, &console_device, &console_device};
enum { debugDevice_SVC, GFX_TOP, GFX_BOTTOM, GSP_BGR8_OES };
static void consoleDebugInit(int device) {
  assert(device == debugDevice_SVC);
  // libctru's API redirects stderr only. stdout must also be handed over.
  devoptab_list[STD_ERR] = &debug_device;
}
static void gfxSetDoubleBuffering(int, bool) {}
static void gfxSetScreenFormat(int, int) {}

extern "C" int PlatformPrintf(const char *format, ...) {
  char message[1024];
  va_list args;
  va_start(args, format);
  const int count = vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (count > 0)
    devoptab_list[STD_OUT]->write(message, std::min((size_t)count, sizeof(message)-1));
  return count;
}

// Real FontManager/FreeType, with Text retaining only its configuration and
// CPU surfaces. InitScreens is the production handoff owner extracted below.
Text::Text() : reporter_(nullptr), fm(nullptr), tr(nullptr) {
  pixelsize = 12;
  margin.left = margin.right = margin.top = margin.bottom = 0;
  screenleft = new u16[1];
  screenright = new u16[1];
  screen = screenleft;
}
Text::~Text() { delete[] screenleft; delete[] screenright; }
void Text::SetScreen(u16 *value) { screen = value; }
void Text::ClearScreen() { *screen = 0xffff; }
int Text::GetStyle() { return TEXT_STYLE_REGULAR; }
u8 Text::GetCharCode(const char *, size_t, u32 *) { std::abort(); }
void TextRenderer::SetHit(bool) { std::abort(); }
int halt(Text *, const char *, int) { std::abort(); }
struct App {
  std::unique_ptr<Text> ts{new Text};
  void InitScreens();
};
#include "stdio_handoff.inc"

int main() {
  App app;
  FontManager fonts(app.ts.get());
  fonts.SetFontDir(std::getenv("TEST_STDIO_FONT_DIR"));
  assert(!fonts.Init());
  // Early diagnostics remain available on the boot console until takeover.
  std::fill(bottom.begin(), bottom.end(), 0x7f);
  PlatformPrintf("boot console\n");
  assert(bottom[0] == 0);

  app.InitScreens();
  std::fill(bottom.begin(), bottom.end(), 0x7f);
  const std::vector<u8> splash = bottom;
  // This real deferred load emits CreateFace's diagnostic while the opening
  // splash is still displayed, both on fresh parse and a cached book's draw.
  FT_Face mono = fonts.GetFace(TEXT_STYLE_MONO);
  assert(mono && FT_IS_FIXED_WIDTH(mono));
  assert(bottom == splash && "late font diagnostics overwrite the loading framebuffer");
  assert(debug_output.find("LiberationMono-Regular.ttf") != std::string::npos);

  PlatformPrintf("late stdout\n");
  const char warning[] = "decoder warning\n";
  devoptab_list[STD_ERR]->write(warning, sizeof(warning)-1);
  assert(bottom == splash && "stdio must not touch the application framebuffer");
  assert(debug_output.find("late stdout") != std::string::npos);
  assert(debug_output.find("decoder warning") != std::string::npos);
  puts("PASS: boot console handoff, real lazy font load and late stdout/stderr preserve the framebuffer");
}

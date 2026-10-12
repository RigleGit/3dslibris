#include <3ds/types.h>

// libctru moves the main thread onto a heap-backed stack, independently of
// the CIA exheader's StackSize. Its 32 KiB default cannot hold the EPUB parser
// (~20 KiB) plus FreeType's CFF glyph interpreter (~16 KiB) and their callers.
// Keep a bounded reserve for cold pagination with CJK fonts in CIA and 3DSX.
extern "C" {
u32 __stacksize__ = 128u * 1024u;
}

// Force-included before test-only -D renames of stdio functions. libstdc++ 15
// <cstdio> does `using ::ftell` and friends, which breaks if <stdio.h> was first
// parsed with the rename active. Load it once with the renames suspended.
#pragma push_macro("ftell")
#pragma push_macro("fflush")
#pragma push_macro("fwrite")
#undef ftell
#undef fflush
#undef fwrite
#include <cstdio>
#pragma pop_macro("fwrite")
#pragma pop_macro("fflush")
#pragma pop_macro("ftell")

// The renamed stdio entry points were previously declared by <stdio.h> itself under
// their new names; declare them explicitly now that stdio is loaded unrenamed.
#ifdef ftell
extern "C" long ObserveMetadataTell(FILE *);
#endif
#ifdef fflush
extern "C" int ObserveCacheFlush(FILE *);
#endif
#ifdef fwrite
extern "C" size_t ObserveCacheWrite(const void *, size_t, size_t, FILE *);
#endif

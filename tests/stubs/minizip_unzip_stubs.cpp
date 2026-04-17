#include "minizip/unzip.h"

extern "C" {

int ZEXPORT unzReadCurrentFile(unzFile, void *, unsigned int) { return 0; }

}

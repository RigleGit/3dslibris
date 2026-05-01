#include "minizip/unzip.h"

extern "C" {

int ZEXPORT unzLocateFile(unzFile, const char *, int) {
  return UNZ_END_OF_LIST_OF_FILE;
}

int ZEXPORT unzGetCurrentFileInfo(unzFile, unz_file_info *, char *, uLong,
                                  void *, uLong, char *, uLong) {
  return UNZ_PARAMERROR;
}

int ZEXPORT unzOpenCurrentFile(unzFile) { return UNZ_PARAMERROR; }

int ZEXPORT unzCloseCurrentFile(unzFile) { return UNZ_OK; }

int ZEXPORT unzReadCurrentFile(unzFile, void *, unsigned int) { return 0; }

}

/*
 * Stub for shared/main.h — omits FreeType so host tests can compile
 * book.cpp and book_xml_parser.cpp without a FreeType installation.
 */
#pragma once
#include "3ds.h"

class Text;
int halt(Text *presenter, int vblanks = -1);
int halt(Text *presenter, const char *msg, int vblanks = -1);

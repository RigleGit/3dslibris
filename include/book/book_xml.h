/*
    3dslibris - book_xml.h
    Internal XML callback declarations for book-style content parsing.

    Summary:
    - Isolates Expat-facing callback declarations from the Book domain model.
    - Keeps XML parser types out of book/book.h and non-XML consumers.
*/

#pragma once

#include "expat.h"

namespace xml {
namespace book {
void start(void *data, const char *el, const char **attr);
void chardata(void *data, const char *txt, int txtlen);
void end(void *data, const char *el);
void instruction(void *data, const char *target, const char *pidata);
int unknown(void *encodingHandlerData, const XML_Char *name,
            XML_Encoding *info);
void fallback(void *data, const XML_Char *s, int len);
} // namespace book
} // namespace xml

namespace xml {
namespace book {
namespace metadata {
void start(void *userdata, const char *el, const char **attr);
void chardata(void *userdata, const char *txt, int txtlen);
void end(void *userdata, const char *el);
} // namespace metadata
} // namespace book
} // namespace xml

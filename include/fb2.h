/*
    3dslibris - fb2.h
    New format support module by Rigle.

    Summary:
    - Declares FB2 helpers currently focused on cover extraction.
    - Complements EPUB pipeline with lightweight FB2 reader support.
*/

#pragma once

#include "book.h"
#include <string>

int fb2_extract_cover(Book *book, const std::string &fb2path);

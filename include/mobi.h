/*
    3dslibris - mobi.h
    New format support module by Rigle.

    Summary:
    - Declares MOBI helpers focused on cover extraction for library thumbnails.
*/

#pragma once

#include "book.h"
#include <string>

int mobi_extract_cover(Book *book, const std::string &mobipath);


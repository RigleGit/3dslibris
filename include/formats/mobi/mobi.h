/*
    3dslibris - mobi.h
    New format support module by Rigle.

    Summary:
    - Declares MOBI helpers focused on cover extraction for library thumbnails.
    - Exposes lightweight PalmDB/image helpers reused by inline MOBI images.
*/

#pragma once

#include "book/book.h"
#include <3ds.h>
#include <stddef.h>
#include <string>
#include <vector>

int mobi_extract_cover(Book *book, const std::string &mobipath);
bool mobi_parse_offsets(const std::string &raw, std::vector<u32> *offsets);
size_t mobi_find_image_start_offset(const u8 *data, size_t len);
bool mobi_extract_image_recindex(const std::string &tag, u16 *recindex_out);
std::string mobi_inline_image_path(u16 recindex);
bool mobi_parse_inline_image_path(const std::string &path, u16 *recindex_out);

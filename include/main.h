/*
 Copyright (C) 2007-2020 Ray Haleblian

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

 To contact the copyright holder: ray@haleblian.com
 */

/*
  3DS port modifications by Rigle (summary):
  - Declares halt() functions used by the 3DS main loop.
*/

#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H
#include "3ds.h"

class Text;

int halt(Text *presenter, int vblanks = -1);
int halt(Text *presenter, const char *msg, int vblanks = -1);

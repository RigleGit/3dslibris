/*
    3dslibris - parser_limits.h

    Shared parser stream limits kept outside main.h so format modules
    can depend on parser constants without pulling app/bootstrap headers.
*/

#pragma once

#include <stddef.h>

namespace parser_limits {

static constexpr size_t kXmlStreamBufferSize = 1024 * 128;

} // namespace parser_limits


#pragma once

#include "mkrender/Config.h"

#include <vector>

// RGBA8 pixels in row order (toon ramp input, captures). A host can use its own type instead (mkrender/Config.h, MKRENDER_HOST_IMAGE).
#ifndef MKRENDER_HOST_IMAGE
namespace mkrender {

struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;
};

} // namespace mkrender
#endif

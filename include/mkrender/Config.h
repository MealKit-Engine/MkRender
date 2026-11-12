#pragma once

// Host types (like Dear ImGui's imconfig): a host that already has its own math, mesh or image types with the same members and
// layout can use them as mkrender's, so its code passes them without conversions. Define MKRENDER_USER_CONFIG as the header name
// (for the library and for every file that includes mkrender, e.g. a PUBLIC compile definition). That header defines, as needed:
//   MKRENDER_HOST_MATH   and puts Vec2, Vec3, Quat, Mat4 and the functions in mkrender/Math.h into namespace mkrender (using-declarations)
//   MKRENDER_HOST_MESH   the same for mkrender/Mesh.h (Vertex, SkinWeights, BoneMatrix, MeshData, MeshId, MaterialData and the constants)
//   MKRENDER_HOST_IMAGE  the same for mkrender/Image.h (Image)
// Without it, mkrender's own definitions are used. The static_asserts in those headers check the layouts either way.
#ifdef MKRENDER_USER_CONFIG
#include MKRENDER_USER_CONFIG
#endif

#include "mkrhi/Rhi.h"

namespace mkrender {

using mkrhi::f32;
using mkrhi::f64;
using mkrhi::i16;
using mkrhi::i32;
using mkrhi::i64;
using mkrhi::i8;
using mkrhi::u16;
using mkrhi::u32;
using mkrhi::u64;
using mkrhi::u8;

} // namespace mkrender

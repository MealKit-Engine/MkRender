#pragma once

#include "mkrender/Math.h"
#include "mkrender/Mesh.h"

#include <string>
#include <vector>

// Skeletal animation of glTF models in the MkRender scene editor: the engine's asset/Animation.h view path (clips baked at 30 Hz,
// samplePose by seconds, the same palette), so an object's "anim" field plays like in the editor. Joint values stay in glTF space; z is flipped in the palette.
namespace mkrender::tool {

inline constexpr u32 kClipFrameRate = 30; // frames of baked clips (Hz)
inline constexpr u32 kMaxJoints = 64;     // GPU palette limit (skins over this are rejected on load)

struct JointPose {
    Vec3 translation;
    Quat rotation;
    Vec3 scale{1, 1, 1};
};

struct Skeleton {
    struct Joint {
        std::string name;
        i32 parent = -1;     // index within joints (parents come first), -1 = root
        JointPose rest;      // rest pose (joints without animation use this)
        Mat4 inverseBind;    // glTF inverse bind matrix (converted to the row vector convention)
    };
    std::vector<Joint> joints;
    Mat4 root = Mat4::identity(); // node transform above the root joint (glTF space, row vectors)
    bool valid() const { return !joints.empty(); }
};

struct AnimationClip {
    std::string name;
    u32 frames = 0;               // at least 1 (last frame = length)
    f32 duration = 0.0f;          // seconds = (frames - 1) / kClipFrameRate
    std::vector<JointPose> poses; // frames × joint count
    const JointPose* frame(u32 f, u32 jointCount) const { return &poses[static_cast<size_t>(f) * jointCount]; }
};


// Pose of the clip at a time (seconds). With loop, wraps by length; otherwise stops at the end. out has one entry per joint
void samplePose(const Skeleton& skeleton, const AnimationClip& clip, f32 seconds, bool loop, std::vector<JointPose>& out);
// Rest pose
void restPose(const Skeleton& skeleton, std::vector<JointPose>& out);
// Joint world matrices (glTF space, row vectors, including the root node transform)
void jointMatrices(const Skeleton& skeleton, const std::vector<JointPose>& pose, std::vector<Mat4>& out);
// View palette: joint world × inverse bind, in MealKit coordinates (z flipped). Passed directly to the renderer's drawSkinned
void skinPalette(const Skeleton& skeleton, const std::vector<JointPose>& pose, std::vector<BoneMatrix>& out);

Mat4 composeTrs(const JointPose& pose); // row vector convention (v * M = scale → rotate → translate)
const AnimationClip* findClip(const std::vector<AnimationClip>& clips, const std::string& name); // nullptr if not found

} // namespace mkrender::tool

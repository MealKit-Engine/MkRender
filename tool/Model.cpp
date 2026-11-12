#include "Model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iterator>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_HDR // sky HDRIs (Assets::environment)
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace mkrender::tool {
namespace {

std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// cgltf file reading through std::filesystem, so UTF-8 paths (non-ASCII folder names) work
cgltf_result cgltfRead(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size, void** data) {
    std::string bytes;
    if (!readFile(pathOf(path), bytes)) return cgltf_result_file_not_found;
    void* buffer = std::malloc(bytes.size() ? bytes.size() : 1);
    if (!buffer) return cgltf_result_out_of_memory;
    std::memcpy(buffer, bytes.data(), bytes.size());
    *size = bytes.size();
    *data = buffer;
    return cgltf_result_success;
}

void cgltfRelease(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

u16 toHalf(f32 value) {
    u32 bits;
    std::memcpy(&bits, &value, 4);
    const u32 sign = (bits >> 16) & 0x8000u;
    const i32 exponent = static_cast<i32>((bits >> 23) & 0xFFu) - 127 + 15;
    u32 mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<u16>(sign);
        mantissa |= 0x800000u;
        return static_cast<u16>(sign | (mantissa >> (14 - exponent)));
    }
    if (exponent >= 31) return static_cast<u16>(sign | 0x7C00u);
    return static_cast<u16>(sign | (static_cast<u32>(exponent) << 10) | (mantissa >> 13));
}

u32 encodeColor(const f32 linear[4]) {
    const auto channel = [](f32 v, bool gamma) {
        const f32 c = std::clamp(v, 0.0f, 1.0f);
        return static_cast<u32>(std::lround((gamma ? std::pow(c, 1.0f / 2.2f) : c) * 255.0f));
    };
    return channel(linear[0], true) | (channel(linear[1], true) << 8) | (channel(linear[2], true) << 16) | (channel(linear[3], false) << 24);
}

// Skin reading state: joint index remap for one skin (the first) (glTF skin.joints index → Skeleton index)
struct SkinState {
    const cgltf_skin* skin = nullptr;
    std::vector<u8> remap;              // skin.joints index → Skeleton index
    std::vector<const cgltf_node*> nodes; // Skeleton index → node
    i32 jointOf(const cgltf_node* n) const { // the closest joint among the node or its ancestors (0 if none: root)
        for (; n; n = n->parent)
            for (size_t i = 0; i < nodes.size(); ++i)
                if (nodes[i] == n) return static_cast<i32>(i);
        return 0;
    }
};

u32 packWeights(const f32 w[4]) {
    f32 sum = w[0] + w[1] + w[2] + w[3];
    if (sum <= 0.0f) return 0xFFu;
    u32 q[4], total = 0;
    for (int i = 0; i < 4; ++i) total += q[i] = static_cast<u32>(std::lround(std::clamp(w[i] / sum, 0.0f, 1.0f) * 255.0f));
    // Make the sum 255 (adjusting the largest weight)
    int big = 0;
    for (int i = 1; i < 4; ++i) if (q[i] > q[big]) big = i;
    q[big] = static_cast<u32>(static_cast<i32>(q[big]) + 255 - static_cast<i32>(total));
    return q[0] | (q[1] << 8) | (q[2] << 16) | (q[3] << 24);
}

Quat nlerpQuat(Quat a, Quat b, f32 t) {
    const f32 d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const f32 s = d < 0.0f ? -1.0f : 1.0f;
    Quat q{a.x + (b.x * s - a.x) * t, a.y + (b.y * s - a.y) * t, a.z + (b.z * s - a.z) * t, a.w + (b.w * s - a.w) * t};
    const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return len > 0.0f ? Quat{q.x / len, q.y / len, q.z / len, q.w / len} : Quat{};
}

// glTF column-major matrix → row vector convention Mat4 (memory as is: column i becomes row i)
Mat4 fromGltf(const f32 m[16]) {
    Mat4 r;
    std::memcpy(&r.m[0][0], m, sizeof(f32) * 16);
    return r;
}

// First skin → Skeleton (reordered so parents come first). false if there are too many joints
bool readSkeleton(const cgltf_skin& skin, SkinState& state, Skeleton& out, std::string& why) {
    const size_t count = skin.joints_count;
    if (count == 0) return false;
    if (count > kMaxJoints) {
        why = std::to_string(count) + " joints (max " + std::to_string(kMaxJoints) + ")";
        return false;
    }
    const auto skinIndex = [&](const cgltf_node* n) -> i32 {
        for (size_t i = 0; i < count; ++i)
            if (skin.joints[i] == n) return static_cast<i32>(i);
        return -1;
    };
    std::vector<i32> parent(count, -1), depth(count, 0); // by skin index
    for (size_t i = 0; i < count; ++i)
        for (const cgltf_node* n = skin.joints[i]->parent; n; n = n->parent) {
            const i32 p = skinIndex(n);
            if (p >= 0 && parent[i] < 0) parent[i] = p;
            if (p >= 0) ++depth[i];
        }
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return depth[a] < depth[b]; });
    state.skin = &skin;
    state.remap.assign(count, 0);
    state.nodes.resize(count);
    for (size_t k = 0; k < count; ++k) {
        state.remap[order[k]] = static_cast<u8>(k);
        state.nodes[k] = skin.joints[order[k]];
    }
    out.joints.resize(count);
    for (size_t k = 0; k < count; ++k) {
        const size_t i = order[k];
        const cgltf_node* n = skin.joints[i];
        Skeleton::Joint& j = out.joints[k];
        j.name = n->name ? n->name : "joint" + std::to_string(k);
        j.parent = parent[i] >= 0 ? static_cast<i32>(state.remap[static_cast<size_t>(parent[i])]) : -1;
        if (n->has_translation) j.rest.translation = {n->translation[0], n->translation[1], n->translation[2]};
        if (n->has_rotation) j.rest.rotation = {n->rotation[0], n->rotation[1], n->rotation[2], n->rotation[3]};
        if (n->has_scale) j.rest.scale = {n->scale[0], n->scale[1], n->scale[2]};
        f32 ib[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (skin.inverse_bind_matrices) cgltf_accessor_read_float(skin.inverse_bind_matrices, i, ib, 16);
        j.inverseBind = fromGltf(ib);
    }
    const cgltf_node* top = state.nodes[0]->parent; // node above the root joint (Blender armature object)
    if (top) {
        f32 m[16];
        cgltf_node_transform_world(top, m);
        out.root = fromGltf(m);
    }
    return true;
}

// glTF animation → clip baked at 30 Hz. Channels that move non-joint nodes and morph weights are dropped.
// Interpolation: linear (rotations nlerp), step, and cubic spline as linear between key values (tangents unused)
AnimationClip bakeClip(const cgltf_animation& anim, const SkinState& state, const Skeleton& skeleton, size_t index) {
    AnimationClip clip;
    clip.name = anim.name && *anim.name ? anim.name : "clip" + std::to_string(index);
    f32 duration = 0.0f;
    for (cgltf_size c = 0; c < anim.channels_count; ++c) {
        const cgltf_accessor* input = anim.channels[c].sampler->input;
        if (input->count) {
            f32 last = 0.0f;
            cgltf_accessor_read_float(input, input->count - 1, &last, 1);
            duration = std::max(duration, last);
        }
    }
    const u32 joints = static_cast<u32>(skeleton.joints.size());
    clip.frames = static_cast<u32>(std::ceil(duration * static_cast<f32>(kClipFrameRate) - 1e-3f)) + 1;
    clip.duration = static_cast<f32>(clip.frames - 1) / static_cast<f32>(kClipFrameRate);
    clip.poses.resize(static_cast<size_t>(clip.frames) * joints);
    for (u32 f = 0; f < clip.frames; ++f)
        for (u32 j = 0; j < joints; ++j) clip.poses[static_cast<size_t>(f) * joints + j] = skeleton.joints[j].rest;
    for (cgltf_size c = 0; c < anim.channels_count; ++c) {
        const cgltf_animation_channel& ch = anim.channels[c];
        i32 joint = -1;
        for (size_t k = 0; k < state.nodes.size(); ++k)
            if (state.nodes[k] == ch.target_node) joint = static_cast<i32>(k);
        const cgltf_animation_path_type path = ch.target_path;
        if (joint < 0 || path == cgltf_animation_path_type_weights || path == cgltf_animation_path_type_invalid) continue;
        const cgltf_animation_sampler& s = *ch.sampler;
        const size_t keys = s.input->count;
        if (keys == 0) continue;
        const int comps = path == cgltf_animation_path_type_rotation ? 4 : 3;
        const bool cubic = s.interpolation == cgltf_interpolation_type_cubic_spline;
        std::vector<f32> times(keys), values(keys * comps);
        for (size_t k = 0; k < keys; ++k) {
            cgltf_accessor_read_float(s.input, k, &times[k], 1);
            cgltf_accessor_read_float(s.output, cubic ? k * 3 + 1 : k, &values[k * comps], comps);
        }
        for (u32 f = 0; f < clip.frames; ++f) {
            const f32 t = static_cast<f32>(f) / static_cast<f32>(kClipFrameRate);
            size_t k = 0;
            while (k + 1 < keys && times[k + 1] <= t) ++k;
            size_t k1 = std::min(k + 1, keys - 1);
            f32 u = 0.0f;
            if (k1 != k && t > times[k] && s.interpolation != cgltf_interpolation_type_step)
                u = std::clamp((t - times[k]) / std::max(times[k1] - times[k], 1e-6f), 0.0f, 1.0f);
            const f32* a = &values[k * comps];
            const f32* b = &values[k1 * comps];
            JointPose& pose = clip.poses[static_cast<size_t>(f) * joints + static_cast<size_t>(joint)];
            if (path == cgltf_animation_path_type_rotation)
                pose.rotation = nlerpQuat({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, u);
            else {
                const Vec3 v = lerp(Vec3{a[0], a[1], a[2]}, Vec3{b[0], b[1], b[2]}, u);
                if (path == cgltf_animation_path_type_translation) pose.translation = v;
                else pose.scale = v;
            }
        }
    }
    return clip;
}

// Style preset material features from Blender custom properties (glTF material extras "mealkit_outline" ...), the engine's asset/ModelCache.cpp
struct FeatureKey {
    const char* key;
    const char* label; // Blender panel (English)
    u32 feature;       // kMaterial*
    bool color;        // three linear color values (otherwise a single number)
    size_t offset;     // value within MaterialData
};
const FeatureKey kFeatureKeys[] = {
    {"outline", "Outline width (m)", kMaterialOutline, false, offsetof(MaterialData, outlineWidth)},
    {"outlineColor", "Outline color", kMaterialOutline, true, offsetof(MaterialData, outlineColor)},
    {"wind", "Wind sway (m, vertex alpha)", kMaterialWind, false, offsetof(MaterialData, windStrength)},
    {"rim", "Rim light strength", kMaterialRim, false, offsetof(MaterialData, rimStrength)},
    {"rimColor", "Rim light color", kMaterialRim, true, offsetof(MaterialData, rimColor)},
    {"dissolve", "Dissolve edge width", kMaterialDissolve, false, offsetof(MaterialData, dissolveEdge)},
    {"dissolveColor", "Dissolve edge glow", kMaterialDissolve, true, offsetof(MaterialData, dissolveColor)},
};

// Top-level keys and values of an extras JSON object. Only numbers/number arrays are read as values (numbers); other values (strings/objects/booleans) get empty numbers.
// Stops where the format breaks (it's JSON written by the Blender exporter, so no strict validation)
struct ExtraValue {
    std::string key;
    std::vector<f32> numbers;
};

std::vector<ExtraValue> parseExtras(const char* json) {
    std::vector<ExtraValue> out;
    const char* p = json;
    const auto ws = [&] { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p; };
    const auto skipString = [&](std::string* text) { // p is the opening quote
        ++p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) ++p;
            if (text) text->push_back(*p);
            ++p;
        }
        if (*p == '"') ++p;
    };
    const auto number = [&](f32& v) {
        char* end = nullptr;
        v = std::strtof(p, &end);
        if (end == p) return false;
        p = end;
        return true;
    };
    std::function<void()> skipValue = [&] {
        ws();
        if (*p == '"') return skipString(nullptr);
        if (*p == '{' || *p == '[') {
            const char close = *p == '{' ? '}' : ']';
            ++p;
            for (ws(); *p && *p != close; ws()) {
                if (*p == ',' || *p == ':') ++p;
                else skipValue();
            }
            if (*p) ++p;
            return;
        }
        while (*p && *p != ',' && *p != '}' && *p != ']') ++p; // numbers, true, false, null
    };
    ws();
    if (*p != '{') return out;
    ++p;
    for (;;) {
        ws();
        if (*p != '"') break;
        ExtraValue v;
        skipString(&v.key);
        ws();
        if (*p != ':') break;
        ++p;
        ws();
        f32 n = 0.0f;
        if (*p == '[') {
            const char* start = p++;
            bool numeric = true;
            for (ws(); *p && *p != ']'; ws()) {
                if (*p == ',') { ++p; continue; }
                if (!number(n)) { numeric = false; break; }
                v.numbers.push_back(n);
            }
            if (!numeric) {
                v.numbers.clear();
                p = start;
                skipValue();
            } else if (*p) {
                ++p;
            }
        } else if (!(*p == '-' || (*p >= '0' && *p <= '9')) || !number(n)) {
            skipValue();
        } else {
            v.numbers.push_back(n);
        }
        out.push_back(std::move(v));
        ws();
        if (*p != ',') break;
        ++p;
    }
    return out;
}

void applyMaterialExtras(const char* json, MaterialData& material, std::vector<std::string>& problems) {
    if (!json) return;
    for (const ExtraValue& v : parseExtras(json)) {
        if (v.key.rfind("mealkit_", 0) != 0) continue;
        const std::string name = v.key.substr(8);
        const FeatureKey* key = nullptr;
        for (const FeatureKey& k : kFeatureKeys)
            if (name == k.key) key = &k;
        if (!key) {
            problems.push_back("unknown engine feature '" + v.key + "' (ignored)");
            continue;
        }
        const size_t want = key->color ? 3 : 1;
        if (v.numbers.size() != want) {
            problems.push_back("'" + v.key + "' needs " + (key->color ? "a color (3 numbers)" : "a number") + " (ignored)");
            continue;
        }
        f32* target = reinterpret_cast<f32*>(reinterpret_cast<u8*>(&material) + key->offset);
        for (size_t i = 0; i < want; ++i) target[i] = std::max(v.numbers[i], 0.0f);
        if (!key->color) material.features |= key->feature; // only main keys turn features on (color alone leaves it off by default)
    }
}

struct Loader {
    ModelData& out;
    std::string path;
    std::vector<const cgltf_material*> materials;
    std::vector<std::pair<const cgltf_image*, u8>> imageKeys; // (image, usage) -> out.images index
    const SkinState* skin = nullptr; // the model's first skin (every vertex gets weights then)
    i32 rigidJoint = 0;              // joint of the node being read (primitives without joints follow it)

    u16 material(const cgltf_material* m) {
        for (size_t i = 0; i < materials.size(); ++i)
            if (materials[i] == m) return static_cast<u16>(i);
        materials.push_back(m);
        return static_cast<u16>(materials.size() - 1);
    }

    void primitive(const cgltf_primitive& prim, const f32 m[16]) {
        if (prim.type != cgltf_primitive_type_triangles) return;
        const cgltf_accessor *positions = nullptr, *normals = nullptr, *colors = nullptr, *texcoords = nullptr;
        const cgltf_accessor *jointsAttr = nullptr, *weightsAttr = nullptr;
        for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
            const cgltf_attribute& a = prim.attributes[i];
            if (a.type == cgltf_attribute_type_position) positions = a.data;
            else if (a.type == cgltf_attribute_type_normal) normals = a.data;
            else if (a.type == cgltf_attribute_type_color && a.index == 0) colors = a.data;
            else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) texcoords = a.data;
            else if (a.type == cgltf_attribute_type_joints && a.index == 0) jointsAttr = a.data;
            else if (a.type == cgltf_attribute_type_weights && a.index == 0) weightsAttr = a.data;
        }
        if (!positions) return;
        const u16 mat = material(prim.material);
        // Column-major matrix; a negative determinant mirrors, so the winding flips once more
        const f32 det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
        MeshData& mesh = out.mesh;
        const u32 base = static_cast<u32>(mesh.vertices.size());
        const bool needNormals = normals == nullptr;
        for (cgltf_size i = 0; i < positions->count; ++i) {
            f32 p[3] = {}, n[3] = {0, 1, 0}, c[4] = {1, 1, 1, 1}, t[2] = {};
            cgltf_accessor_read_float(positions, i, p, 3);
            if (normals) cgltf_accessor_read_float(normals, i, n, 3);
            if (colors) cgltf_accessor_read_float(colors, i, c, cgltf_num_components(colors->type));
            if (texcoords) cgltf_accessor_read_float(texcoords, i, t, 2);
            const Vec3 wp{m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
                          m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
            const Vec3 wn = normalize(Vec3{m[0] * n[0] + m[4] * n[1] + m[8] * n[2], m[1] * n[0] + m[5] * n[1] + m[9] * n[2],
                                           m[2] * n[0] + m[6] * n[1] + m[10] * n[2]});
            // glTF is right-handed: flip z (texture coordinates stay, v down like DX12)
            Vertex v;
            v.position = {wp.x, wp.y, -wp.z};
            v.normal = {wn.x, wn.y, -wn.z};
            v.color = encodeColor(c);
            v.uv = packHalf2(t[0], t[1]);
            v.material = mat;
            mesh.vertices.push_back(v);
            if (skin) {
                SkinWeights sw;
                sw.joints = static_cast<u32>(rigidJoint);
                if (jointsAttr && weightsAttr) {
                    cgltf_uint j[4] = {};
                    f32 w[4] = {};
                    cgltf_accessor_read_uint(jointsAttr, i, j, 4);
                    cgltf_accessor_read_float(weightsAttr, i, w, 4);
                    sw.joints = 0;
                    for (int k = 0; k < 4; ++k) sw.joints |= static_cast<u32>(j[k] < skin->remap.size() ? skin->remap[j[k]] : 0) << (8 * k);
                    sw.weights = packWeights(w);
                }
                mesh.skin.push_back(sw);
            }
        }
        const cgltf_size indexCount = prim.indices ? prim.indices->count : positions->count;
        const bool flip = det >= 0.0f;
        const size_t firstIndex = mesh.indices.size();
        for (cgltf_size i = 0; i + 2 < indexCount; i += 3) {
            u32 tri[3];
            for (int k = 0; k < 3; ++k) tri[k] = static_cast<u32>(prim.indices ? cgltf_accessor_read_index(prim.indices, i + k) : i + k);
            mesh.indices.push_back(base + tri[0]);
            mesh.indices.push_back(base + (flip ? tri[2] : tri[1]));
            mesh.indices.push_back(base + (flip ? tri[1] : tri[2]));
        }
        if (needNormals) { // flat-ish normals from the triangles of this primitive
            for (size_t v = base; v < mesh.vertices.size(); ++v) mesh.vertices[v].normal = {};
            for (size_t i = firstIndex; i + 2 < mesh.indices.size(); i += 3) {
                Vertex& a = mesh.vertices[mesh.indices[i]];
                Vertex& b = mesh.vertices[mesh.indices[i + 1]];
                Vertex& c = mesh.vertices[mesh.indices[i + 2]];
                const Vec3 n = cross(b.position - a.position, c.position - a.position);
                a.normal += n;
                b.normal += n;
                c.normal += n;
            }
            for (size_t v = base; v < mesh.vertices.size(); ++v) mesh.vertices[v].normal = normalize(mesh.vertices[v].normal);
        }
    }

    void node(const cgltf_node* n) {
        if (n->mesh) {
            f32 world[16];
            cgltf_node_transform_world(n, world);
            if (n->skin) { // skinned mesh vertices are in bind space: the node transform isn't used
                std::memset(world, 0, sizeof(world));
                world[0] = world[5] = world[10] = world[15] = 1.0f;
            }
            rigidJoint = skin ? skin->jointOf(n) : 0;
            for (cgltf_size i = 0; i < n->mesh->primitives_count; ++i) primitive(n->mesh->primitives[i], world);
        }
        for (cgltf_size i = 0; i < n->children_count; ++i) node(n->children[i]);
    }

    u32 texture(const cgltf_texture_view& view, u8 usage) { // 0 color (sRGB), 1 linear, 2 normal
        const bool srgb = usage == 0;
        const cgltf_image* image = view.texture ? view.texture->image : nullptr;
        if (!image) return kNoTexture;
        for (size_t i = 0; i < imageKeys.size(); ++i)
            if (imageKeys[i].first == image && imageKeys[i].second == usage) return static_cast<u32>(i);
        const std::string name = image->name ? image->name : image->uri ? image->uri : "?";
        std::string storage;
        const u8* bytes = nullptr;
        size_t size = 0;
        if (image->buffer_view && image->buffer_view->buffer->data) {
            bytes = static_cast<const u8*>(image->buffer_view->buffer->data) + image->buffer_view->offset;
            size = image->buffer_view->size;
        } else if (image->uri && std::strncmp(image->uri, "data:", 5) != 0) {
            std::string uri = image->uri;
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            if (readFile(pathOf(path).parent_path() / pathOf(uri), storage)) {
                bytes = reinterpret_cast<const u8*>(storage.data());
                size = storage.size();
            }
        }
        TextureImage t;
        t.srgb = srgb;
        t.usage = usage;
        int w = 0, h = 0, comp = 0;
        stbi_uc* pixels = bytes ? stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &comp, 4) : nullptr;
        if (!pixels) {
            out.warnings.push_back("texture " + name + ": " + (bytes ? stbi_failure_reason() : "image data not found (data URIs are not read)"));
            return kNoTexture;
        }
        t.width = static_cast<u32>(w);
        t.height = static_cast<u32>(h);
        t.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
        stbi_image_free(pixels);
        t.encoded.assign(reinterpret_cast<const char*>(bytes), size);
        imageKeys.emplace_back(image, usage);
        out.images.push_back(std::move(t));
        return static_cast<u32>(out.images.size() - 1);
    }

    void warnUnsupported(const cgltf_material& m) {
        const std::string name = m.name ? m.name : "?";
        const auto warn = [&](const char* what) { out.warnings.push_back("material " + name + ": " + what + " is not supported (ignored)"); };
        if (m.has_pbr_specular_glossiness) warn("specular-glossiness");
        if (m.has_clearcoat) warn("clearcoat");
        if (m.has_transmission || m.has_diffuse_transmission || m.has_volume) warn("transmission");
        if (m.has_sheen) warn("sheen");
        if (m.alpha_mode == cgltf_alpha_mode_blend) warn("alpha blend (drawn opaque)");
        if (m.double_sided) warn("double sided (back faces are culled)");
        if (m.occlusion_texture.texture) warn("occlusion texture");
    }
};

// Front faces: cross(b - a, c - a) along the outward normal (D3D clockwise in this left-handed space, like the engine's meshes)
void fixWinding(MeshData& mesh) {
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vertex& a = mesh.vertices[mesh.indices[i]];
        const Vertex& b = mesh.vertices[mesh.indices[i + 1]];
        const Vertex& c = mesh.vertices[mesh.indices[i + 2]];
        if (dot(cross(b.position - a.position, c.position - a.position), a.normal + b.normal + c.normal) < 0.0f)
            std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
    }
}

} // namespace

u32 packHalf2(f32 u, f32 v) { return static_cast<u32>(toHalf(u)) | (static_cast<u32>(toHalf(v)) << 16); }

void computeBounds(ModelData& model) {
    model.boundsMin = {1e30f, 1e30f, 1e30f};
    model.boundsMax = {-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : model.mesh.vertices) {
        const Vec3 p = v.position;
        model.boundsMin = {std::min(model.boundsMin.x, p.x), std::min(model.boundsMin.y, p.y), std::min(model.boundsMin.z, p.z)};
        model.boundsMax = {std::max(model.boundsMax.x, p.x), std::max(model.boundsMax.y, p.y), std::max(model.boundsMax.z, p.z)};
    }
    if (model.mesh.vertices.empty()) model.boundsMin = model.boundsMax = {};
}

bool loadGltf(const std::string& path, ModelData& out, std::string& error) {
    out = {};
    cgltf_options options{};
    options.file.read = cgltfRead;
    options.file.release = cgltfRelease;
    cgltf_data* data = nullptr;
    cgltf_result r = cgltf_parse_file(&options, path.c_str(), &data);
    if (r == cgltf_result_success) r = cgltf_load_buffers(&options, data, path.c_str());
    if (r == cgltf_result_success) r = cgltf_validate(data);
    if (r != cgltf_result_success) {
        error = "invalid or unreadable glTF (cgltf error " + std::to_string(static_cast<int>(r)) + ")";
        if (data) cgltf_free(data);
        return false;
    }
    Loader loader{out, path, {}, {}};
    SkinState skinState;
    if (data->skins_count) {
        std::string why;
        if (readSkeleton(data->skins[0], skinState, out.skeleton, why)) {
            for (size_t a = 0; a < data->animations_count; ++a) out.clips.push_back(bakeClip(data->animations[a], skinState, out.skeleton, a));
            loader.skin = &skinState;
            if (data->skins_count > 1) out.warnings.push_back(std::to_string(data->skins_count) + " skins: only the first is animated");
        } else if (!why.empty()) {
            out.warnings.push_back("skin not animated: " + why);
        }
    }
    const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) loader.node(scene->nodes[i]);
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
            if (!data->nodes[i].parent) loader.node(&data->nodes[i]);
    }
    for (const cgltf_material* m : loader.materials) {
        MaterialData md;
        std::string name = "(default)";
        if (m) {
            loader.warnUnsupported(*m);
            name = m->name && *m->name ? m->name : "material " + std::to_string(out.materials.size());
            const cgltf_pbr_metallic_roughness& pbr = m->pbr_metallic_roughness;
            if (m->has_pbr_metallic_roughness) {
                std::memcpy(md.baseColor, pbr.base_color_factor, sizeof(md.baseColor));
                md.metallic = pbr.metallic_factor;
                md.roughness = pbr.roughness_factor;
                md.baseColorTexture = loader.texture(pbr.base_color_texture, 0);
                md.metallicRoughnessTexture = loader.texture(pbr.metallic_roughness_texture, 1);
            }
            std::memcpy(md.emissive, m->emissive_factor, sizeof(md.emissive));
            if (m->has_emissive_strength)
                for (f32& e : md.emissive) e *= m->emissive_strength.emissive_strength;
            md.emissiveTexture = loader.texture(m->emissive_texture, 0);
            md.normalTexture = loader.texture(m->normal_texture, 2);
            md.normalScale = m->normal_texture.scale;
            if (m->alpha_mode == cgltf_alpha_mode_mask) md.alphaCutoff = std::max(m->alpha_cutoff, 1e-3f);
            std::vector<std::string> problems;
            applyMaterialExtras(m->extras.data, md, problems);
            for (const std::string& p : problems) out.warnings.push_back("material " + name + ": " + p);
        }
        out.materials.push_back(md);
        out.materialNames.push_back(name);
    }
    cgltf_free(data);
    if (out.mesh.indices.empty()) {
        error = "no triangle meshes";
        return false;
    }
    computeBounds(out);
    return true;
}

MeshData makeSphere(f32 radius, u32 segments, u32 rings, u16 material) {
    MeshData mesh;
    constexpr f32 kPi = 3.14159265358979f;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 v = static_cast<f32>(r) / static_cast<f32>(rings);
        const f32 phi = v * kPi;
        for (u32 s = 0; s <= segments; ++s) {
            const f32 u = static_cast<f32>(s) / static_cast<f32>(segments);
            const f32 theta = u * 2.0f * kPi;
            const Vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            Vertex vert;
            vert.position = n * radius;
            vert.normal = n;
            vert.uv = packHalf2(u, v);
            vert.material = material;
            mesh.vertices.push_back(vert);
        }
    }
    for (u32 r = 0; r < rings; ++r)
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1) + s, b = a + segments + 1;
            mesh.indices.insert(mesh.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    fixWinding(mesh);
    return mesh;
}

MeshData makeBox(Vec3 h, u16 material) {
    MeshData mesh;
    const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3& n : normals) {
        // Two axes on the face, chosen so (u x v) points along n
        const Vec3 up = std::abs(n.y) > 0.5f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        const Vec3 u = cross(up, n), v = cross(n, u);
        const u32 base = static_cast<u32>(mesh.vertices.size());
        const f32 corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : corners) {
            const Vec3 p = n + u * c[0] + v * c[1];
            Vertex vert;
            vert.position = {p.x * h.x, p.y * h.y, p.z * h.z};
            vert.normal = n;
            vert.uv = packHalf2(c[0] * 0.5f + 0.5f, 0.5f - c[1] * 0.5f);
            vert.material = material;
            mesh.vertices.push_back(vert);
        }
        mesh.indices.insert(mesh.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
    fixWinding(mesh);
    return mesh;
}

MeshData makePlane(f32 half, u16 material) {
    MeshData mesh;
    const f32 corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const auto& c : corners) {
        Vertex v;
        v.position = {c[0] * half, 0.0f, c[1] * half};
        v.normal = {0, 1, 0};
        v.uv = packHalf2(c[0] * 0.5f + 0.5f, c[1] * 0.5f + 0.5f);
        v.material = material;
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 2, 1, 0, 3, 2};
    fixWinding(mesh);
    return mesh;
}

std::vector<u8> buildMips(const TextureImage& image, u32& mipLevels) {
    std::vector<u8> out(image.rgba);
    u32 w = image.width, h = image.height;
    mipLevels = 1;
    std::vector<f32> toLinear(256);
    for (int i = 0; i < 256; ++i) toLinear[i] = image.srgb ? std::pow(i / 255.0f, 2.2f) : i / 255.0f;
    size_t prev = 0;
    while (w > 1 || h > 1) {
        const u32 nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        const size_t start = out.size();
        out.resize(start + static_cast<size_t>(nw) * nh * 4);
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x)
                for (u32 c = 0; c < 4; ++c) {
                    f32 sum = 0.0f;
                    for (u32 dy = 0; dy < 2; ++dy)
                        for (u32 dx = 0; dx < 2; ++dx) {
                            const u32 sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                            const u8 value = out[prev + (static_cast<size_t>(sy) * w + sx) * 4 + c];
                            sum += c == 3 ? value / 255.0f : toLinear[value];
                        }
                    f32 avg = sum * 0.25f;
                    if (c != 3 && image.srgb) avg = std::pow(avg, 1.0f / 2.2f);
                    out[start + (static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<u8>(std::lround(std::clamp(avg, 0.0f, 1.0f) * 255.0f));
                }
        prev = start;
        w = nw;
        h = nh;
        ++mipLevels;
    }
    return out;
}

} // namespace mkrender::tool

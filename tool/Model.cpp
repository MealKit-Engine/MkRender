#include "Model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

struct Loader {
    ModelData& out;
    std::string path;
    std::vector<const cgltf_material*> materials;
    std::vector<std::pair<const cgltf_image*, bool>> imageKeys; // (image, srgb) -> out.images index

    u16 material(const cgltf_material* m) {
        for (size_t i = 0; i < materials.size(); ++i)
            if (materials[i] == m) return static_cast<u16>(i);
        materials.push_back(m);
        return static_cast<u16>(materials.size() - 1);
    }

    void primitive(const cgltf_primitive& prim, const f32 m[16]) {
        if (prim.type != cgltf_primitive_type_triangles) return;
        const cgltf_accessor *positions = nullptr, *normals = nullptr, *colors = nullptr, *texcoords = nullptr;
        for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
            const cgltf_attribute& a = prim.attributes[i];
            if (a.type == cgltf_attribute_type_position) positions = a.data;
            else if (a.type == cgltf_attribute_type_normal) normals = a.data;
            else if (a.type == cgltf_attribute_type_color && a.index == 0) colors = a.data;
            else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) texcoords = a.data;
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
            for (cgltf_size i = 0; i < n->mesh->primitives_count; ++i) primitive(n->mesh->primitives[i], world);
        }
        for (cgltf_size i = 0; i < n->children_count; ++i) node(n->children[i]);
    }

    u32 texture(const cgltf_texture_view& view, bool srgb) {
        const cgltf_image* image = view.texture ? view.texture->image : nullptr;
        if (!image) return kNoTexture;
        for (size_t i = 0; i < imageKeys.size(); ++i)
            if (imageKeys[i].first == image && imageKeys[i].second == srgb) return static_cast<u32>(i);
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
        imageKeys.emplace_back(image, srgb);
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
    const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) loader.node(scene->nodes[i]);
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
            if (!data->nodes[i].parent) loader.node(&data->nodes[i]);
    }
    if (data->skins_count) out.warnings.push_back("skins and animations are shown in the bind pose");
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
                md.baseColorTexture = loader.texture(pbr.base_color_texture, true);
                md.metallicRoughnessTexture = loader.texture(pbr.metallic_roughness_texture, false);
            }
            std::memcpy(md.emissive, m->emissive_factor, sizeof(md.emissive));
            if (m->has_emissive_strength)
                for (f32& e : md.emissive) e *= m->emissive_strength.emissive_strength;
            md.emissiveTexture = loader.texture(m->emissive_texture, true);
            md.normalTexture = loader.texture(m->normal_texture, false);
            md.normalScale = m->normal_texture.scale;
            if (m->alpha_mode == cgltf_alpha_mode_mask) md.alphaCutoff = std::max(m->alpha_cutoff, 1e-3f);
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

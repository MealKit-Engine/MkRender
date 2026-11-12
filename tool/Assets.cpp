#include "Assets.h"

#include "Model.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace mkrender::tool {
namespace {

std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8Of(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 16-bit (or 8-bit) grayscale PNG -> square sample grid, z = 0 at the -z edge (the image's top row is +z), like asset/Terrain.h
bool loadHeightmap(const std::string& file, u32& size, std::vector<u16>& samples, std::string& error) {
    std::ifstream in(pathOf(file), std::ios::binary);
    if (!in) {
        error = "cannot open " + file;
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int w = 0, h = 0, comp = 0;
    stbi_us* pixels = stbi_load_16_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &comp, 1);
    if (!pixels) {
        error = std::string("heightmap: ") + stbi_failure_reason();
        return false;
    }
    if (w != h || w < 2) {
        stbi_image_free(pixels);
        error = "heightmap must be square";
        return false;
    }
    size = static_cast<u32>(w);
    samples.resize(static_cast<size_t>(w) * h);
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x) samples[static_cast<size_t>(z) * w + x] = pixels[static_cast<size_t>(h - 1 - z) * w + x];
    stbi_image_free(pixels);
    return true;
}

MeshData terrainMesh(u32 n, const std::vector<u16>& s, Vec3 size) {
    MeshData mesh;
    const auto pos = [&](u32 x, u32 z) {
        return Vec3{-size.x * 0.5f + static_cast<f32>(x) * size.x / static_cast<f32>(n - 1),
                    -size.y * 0.5f + static_cast<f32>(s[static_cast<size_t>(z) * n + x]) / 65535.0f * size.y,
                    -size.z * 0.5f + static_cast<f32>(z) * size.z / static_cast<f32>(n - 1)};
    };
    mesh.vertices.resize(static_cast<size_t>(n) * n);
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
            Vertex& v = mesh.vertices[static_cast<size_t>(z) * n + x];
            v.position = pos(x, z);
            const Vec3 dx = pos(std::min(x + 1, n - 1), z) - pos(x > 0 ? x - 1 : 0, z);
            const Vec3 dz = pos(x, std::min(z + 1, n - 1)) - pos(x, z > 0 ? z - 1 : 0);
            v.normal = normalize(cross(dz, dx));
            v.uv = packHalf2(static_cast<f32>(x) / static_cast<f32>(n - 1), static_cast<f32>(z) / static_cast<f32>(n - 1));
        }
    for (u32 z = 0; z + 1 < n; ++z)
        for (u32 x = 0; x + 1 < n; ++x) {
            const u32 a = z * n + x, b = a + 1, c = a + n, d = c + 1;
            // Front faces: cross(b - a, c - a) points up (the same rule as the engine's meshes)
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    return mesh;
}

} // namespace

void Assets::init(Renderer& renderer) {
    m_renderer = &renderer;
    m_box = renderer.createMesh(makeBox({0.5f, 0.5f, 0.5f}, 0));
}

void Assets::setAssetsDir(const std::string& dir) {
    if (dir == m_assetsDir) return;
    m_assetsDir = dir;
    m_models.clear();
    m_terrains.clear();
    m_scanned = false;
}

std::string Assets::resolve(const std::string& path) const {
    const std::filesystem::path p = pathOf(path);
    if (p.is_absolute() || m_assetsDir.empty()) return path;
    return utf8Of((pathOf(m_assetsDir) / p).lexically_normal());
}

std::string Assets::relative(const std::string& absolute) const {
    if (m_assetsDir.empty()) return absolute;
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(pathOf(absolute), pathOf(m_assetsDir), ec);
    const std::string text = utf8Of(rel.generic_u8string());
    if (ec || text.empty() || text.rfind("..", 0) == 0) return absolute;
    return text;
}

const ModelEntry& Assets::model(const std::string& path) {
    auto it = m_models.find(path);
    if (it != m_models.end()) return it->second;
    ModelEntry& entry = m_models[path];
    ModelData data;
    if (!loadGltf(resolve(path), data, entry.error)) return entry;
    Renderer& r = *m_renderer;
    std::vector<u32> textures;
    for (const TextureImage& image : data.images) {
        mkrhi::TextureInit init;
        const std::vector<u8> mips = buildMips(image, init.mipLevels);
        init.width = image.width;
        init.height = image.height;
        init.format = image.srgb ? mkrhi::Format::Rgba8Srgb : mkrhi::Format::Rgba8Unorm;
        init.data = mips.data();
        textures.push_back(r.createTexture(init));
    }
    const auto tex = [&](u32 i) { return i < textures.size() ? textures[i] : kNoTexture; };
    std::vector<u32> materials;
    for (MaterialData d : data.materials) {
        d.baseColorTexture = tex(d.baseColorTexture);
        d.normalTexture = tex(d.normalTexture);
        d.metallicRoughnessTexture = tex(d.metallicRoughnessTexture);
        d.emissiveTexture = tex(d.emissiveTexture);
        materials.push_back(r.createMaterial(d));
    }
    for (Vertex& v : data.mesh.vertices) v.material = static_cast<u16>(v.material < materials.size() ? materials[v.material] : 0);
    entry.mesh = r.createMesh(data.mesh);
    entry.boundsMin = data.boundsMin;
    entry.boundsMax = data.boundsMax;
    entry.ok = true;
    entry.warnings = data.warnings;
    entry.triangles = static_cast<u32>(data.mesh.indices.size() / 3);
    entry.materials = static_cast<u32>(data.materials.size());
    entry.textures = static_cast<u32>(data.images.size());
    return entry;
}

void Assets::reload(const std::string& path) {
    m_renderer->waitIdle();
    m_models.erase(path);
}

void Assets::reloadAll() {
    m_renderer->waitIdle();
    m_models.clear();
    m_terrains.clear();
}

MeshId Assets::terrain(u32 entityId, const std::string& heightmap, Vec3 size, std::string* error) {
    TerrainEntry& t = m_terrains[entityId];
    const bool same = t.heightmap == heightmap && t.size.x == size.x && t.size.y == size.y && t.size.z == size.z;
    if (!same || (t.mesh == kNoMesh && t.error.empty())) {
        t.heightmap = heightmap;
        t.size = size;
        t.error.clear();
        u32 n = 0;
        std::vector<u16> samples;
        if (heightmap.empty()) {
            n = 2;
            samples.assign(4, 0);
        } else if (!loadHeightmap(resolve(heightmap), n, samples, t.error)) {
            n = 0;
        }
        if (n) {
            const MeshData mesh = terrainMesh(n, samples, size);
            if (t.mesh == kNoMesh) t.mesh = m_renderer->createMesh(mesh);
            else m_renderer->replaceMesh(t.mesh, mesh);
        }
    }
    if (error) *error = t.error;
    return t.error.empty() ? t.mesh : kNoMesh;
}

const std::vector<std::string>& Assets::modelFiles() {
    if (!m_scanned) {
        m_scanned = true;
        m_modelFiles = files(".glb");
        for (const std::string& f : files(".gltf")) m_modelFiles.push_back(f);
        std::sort(m_modelFiles.begin(), m_modelFiles.end());
    }
    return m_modelFiles;
}

std::vector<std::string> Assets::files(const char* extension) const {
    std::vector<std::string> out;
    if (m_assetsDir.empty()) return out;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(pathOf(m_assetsDir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (lower(utf8Of(it->path().extension())) != extension) continue;
        out.push_back(relative(utf8Of(it->path())));
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace mkrender::tool

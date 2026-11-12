#include "Assets.h"

#include "Model.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
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
    m_images.clear();
    m_sprites.clear();
    m_scatters.clear();
}

const SpriteEntry& Assets::sprite(const std::string& image, int columns, int rows, int cell, f32 pivotX, f32 pivotY, bool flipX, bool flipY) {
    columns = std::max(columns, 1);
    rows = std::max(rows, 1);
    cell = std::clamp(cell, 0, columns * rows - 1);
    char key[96];
    std::snprintf(key, sizeof(key), "|%d|%d|%d|%g|%g|%d%d", columns, rows, cell, pivotX, pivotY, flipX ? 1 : 0, flipY ? 1 : 0);
    auto found = m_sprites.find(image + key);
    if (found != m_sprites.end()) return found->second;
    SpriteEntry& s = m_sprites[image + key];
    ImageEntry& img = m_images[image];
    if (!img.width && img.error.empty()) {
        std::ifstream in(pathOf(resolve(image)), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        int w = 0, h = 0, comp = 0;
        stbi_uc* pixels = bytes.empty() ? nullptr
                                        : stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
        if (!pixels) {
            img.error = in ? std::string("image: ") + stbi_failure_reason() : "cannot open " + image;
        } else {
            TextureImage t;
            t.width = static_cast<u32>(w);
            t.height = static_cast<u32>(h);
            t.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
            stbi_image_free(pixels);
            mkrhi::TextureInit init;
            const std::vector<u8> mips = buildMips(t, init.mipLevels);
            init.width = t.width;
            init.height = t.height;
            init.format = mkrhi::Format::Rgba8Srgb;
            init.data = mips.data();
            MaterialData m;
            m.baseColorTexture = m_renderer->createTexture(init);
            m.alphaCutoff = 0.5f;
            img.material = m_renderer->createMaterial(m);
            img.width = t.width;
            img.height = t.height;
        }
    }
    if (!img.error.empty()) {
        s.error = img.error;
        return s;
    }
    s.cellWidth = img.width / static_cast<u32>(columns);
    s.cellHeight = img.height / static_cast<u32>(rows);
    const int col = cell % columns, row = cell / columns;
    f32 u0 = static_cast<f32>(col) / columns, u1 = static_cast<f32>(col + 1) / columns;
    f32 v0 = static_cast<f32>(row) / rows, v1 = static_cast<f32>(row + 1) / rows; // v0 = top
    if (flipX) std::swap(u0, u1);
    if (flipY) std::swap(v0, v1);
    const f32 x0 = -pivotX, x1 = 1.0f - pivotX, y0 = -pivotY, y1 = 1.0f - pivotY;
    MeshData mesh;
    const auto vertex = [&](f32 x, f32 y, f32 u, f32 v) {
        Vertex vx;
        vx.position = {x, y, 0.0f};
        vx.normal = {0.0f, 0.0f, -1.0f};
        vx.uv = packHalf2(u, v);
        vx.material = static_cast<u16>(img.material);
        mesh.vertices.push_back(vx);
    };
    vertex(x0, y0, u0, v1); // bottom left
    vertex(x1, y0, u1, v1);
    vertex(x1, y1, u1, v0);
    vertex(x0, y1, u0, v0);
    mesh.indices = {0, 2, 1, 0, 3, 2}; // front face toward -z
    s.mesh = m_renderer->createMesh(mesh);
    return s;
}

const ScatterEntry& Assets::scatter(const std::string& file) {
    ScatterFile& f = m_scatters[file];
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(pathOf(resolve(file)), ec);
    const long long time = ec ? -1 : static_cast<long long>(t.time_since_epoch().count());
    if (time == f.time && (time != 0)) return f.entry;
    f.time = time;
    f.entry = {};
    if (time == -1) {
        f.entry.error = "cannot open " + file;
        return f.entry;
    }
    std::ifstream in(pathOf(resolve(file)), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        ScatterItem item;
        if (std::sscanf(line.c_str(), " i %f %f %f %f %f", &item.position.x, &item.position.y, &item.position.z, &item.yaw, &item.scale) == 5)
            f.entry.items.push_back(item);
    }
    return f.entry;
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

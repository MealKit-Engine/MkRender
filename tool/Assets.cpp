#include "Assets.h"

#include "Model.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <iterator>
#include <system_error>

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

long long writeTime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(pathOf(path), ec);
    return ec ? -1 : static_cast<long long>(t.time_since_epoch().count());
}

bool readBytes(const std::string& path, std::string& out) {
    std::ifstream in(pathOf(path), std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

bool writeBytes(const std::string& path, const std::string& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(pathOf(path).parent_path(), ec);
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(pathOf(temp), std::ios::binary | std::ios::trunc);
        if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return false;
    }
    std::filesystem::rename(pathOf(temp), pathOf(path), ec); // replaces the old file
    return !ec;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
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
    m_environments.clear();
    m_luts.clear();
    m_probes.clear();
    m_scanned = false;
}

std::string Assets::cacheDir() const {
    if (m_assetsDir.empty()) return {};
    return utf8Of(pathOf(m_assetsDir).parent_path() / "build" / "cache");
}

const GpuEntry& Assets::environment(const std::string& hdri) {
    TimedEntry& t = m_environments[hdri];
    const std::string file = resolve(hdri);
    const long long time = writeTime(file);
    if (time == t.time && (t.entry.id || !t.entry.error.empty())) return t.entry;
    t.time = time;
    t.entry = {};
    std::string bytes;
    if (!readBytes(file, bytes)) {
        t.entry.error = "cannot open " + hdri;
        return t.entry;
    }
    EnvironmentMap map;
    const std::string cache = cacheDir().empty() ? std::string() : cacheDir() + "/environments/" + environmentCacheName(bytes);
    std::string cached;
    if (cache.empty() || !readBytes(cache, cached) || !readEnvironmentFile(cached, map)) {
        int w = 0, h = 0, comp = 0;
        f32* rgb = stbi_loadf_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &comp, 3);
        if (!rgb) {
            t.entry.error = std::string("HDR image: ") + stbi_failure_reason();
            return t.entry;
        }
        bakeEnvironment(rgb, static_cast<u32>(w), static_cast<u32>(h), map);
        stbi_image_free(rgb);
        if (!cache.empty() && !writeBytes(cache, environmentFileBytes(map))) t.entry.error = "baked, but cannot write " + cache;
    }
    EnvironmentData data;
    std::memcpy(data.sh, map.sh, sizeof(data.sh));
    data.skySize = map.skySize;
    data.sky = map.sky.data();
    data.reflectSize = map.reflectSize;
    data.reflectMips = map.reflectMips;
    data.reflect = map.reflect.data();
    t.entry.id = m_renderer->createEnvironment(data);
    return t.entry;
}

const GpuEntry& Assets::lut(const std::string& cube) {
    TimedEntry& t = m_luts[cube];
    const std::string file = resolve(cube);
    const long long time = writeTime(file);
    if (time == t.time && (t.entry.id || !t.entry.error.empty())) return t.entry;
    t.time = time;
    t.entry = {};
    std::string text;
    std::vector<f32> cells;
    if (!readBytes(file, text)) t.entry.error = "cannot open " + cube;
    else if (parseCubeLut(text, kLutSize, cells, t.entry.error)) t.entry.id = m_renderer->createLut(cells.data());
    return t.entry;
}

const GpuEntry& Assets::probe(const std::string& path) {
    TimedEntry& t = m_probes[path];
    const std::string file = resolve(path);
    const long long time = writeTime(file);
    if (time == t.time && (t.entry.id || !t.entry.error.empty())) return t.entry;
    t.time = time;
    t.entry = {};
    std::string bytes;
    ProbeMap map;
    if (!readBytes(file, bytes)) t.entry.error = "not baked yet (" + path + ")";
    else if (readProbeFile(bytes, map, t.entry.error)) t.entry.id = m_renderer->createProbe(map.size, map.mips, map.reflect.data());
    return t.entry;
}

u32 Assets::uploadTexture(const TextureImage& image) {
    mkrhi::TextureInit init;
    if (m_compress && !image.encoded.empty()) {
        const TextureUsage usage = static_cast<TextureUsage>(image.usage);
        const std::string cache = cacheDir().empty() ? std::string()
                                                     : cacheDir() + "/textures/" + textureCacheName(image.encoded.data(), image.encoded.size(), usage);
        std::string bytes;
        CompressedTexture c;
        bool ok = !cache.empty() && readBytes(cache, bytes) && readTextureFile(bytes, c);
        if (!ok && compressTexture(image.rgba.data(), image.width, image.height, usage, c)) {
            ok = true;
            if (!cache.empty()) writeBytes(cache, textureFileBytes(c));
        }
        if (ok) {
            init.width = c.width;
            init.height = c.height;
            init.mipLevels = c.mipLevels;
            init.format = c.format;
            init.data = c.data.data();
            return m_renderer->createTexture(init);
        }
    }
    const std::vector<u8> mips = buildMips(image, init.mipLevels);
    init.width = image.width;
    init.height = image.height;
    init.format = image.srgb ? mkrhi::Format::Rgba8Srgb : mkrhi::Format::Rgba8Unorm;
    init.data = mips.data();
    return m_renderer->createTexture(init);
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
    for (const TextureImage& image : data.images) textures.push_back(uploadTexture(image));
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
    m_environments.clear();
    m_luts.clear();
    m_probes.clear();
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
    if (f.entry.dirty) return f.entry; // painted: the memory copy wins until saved
    const long long time = writeTime(resolve(file));
    if (time == f.time && time != 0) return f.entry;
    f.time = time;
    f.entry = {};
    std::string text;
    if (time == -1 || !readBytes(resolve(file), text)) {
        f.entry.error = "cannot open " + file;
        return f.entry;
    }
    readScatter(text, f.entry.items, f.entry.error);
    return f.entry;
}

ScatterEntry& Assets::scatterEdit(const std::string& file) {
    scatter(file);
    ScatterEntry& e = m_scatters[file].entry;
    e.error.clear(); // a missing file is a new layer
    e.dirty = true;
    return e;
}

Assets::HeightmapFile& Assets::heightmapFile(const std::string& path) {
    HeightmapFile& h = m_heightmaps[path];
    if (h.dirty) return h;
    const long long time = writeTime(resolve(path));
    if (time == h.time && (h.map.valid() || !h.error.empty())) return h;
    h.time = time;
    h.error.clear();
    h.map = {};
    ++h.generation;
    std::string bytes;
    if (!readBytes(resolve(path), bytes)) h.error = "cannot open " + path;
    else if (!decodeHeightmap(reinterpret_cast<const u8*>(bytes.data()), bytes.size(), h.map, h.error)) h.error = path + ": " + h.error;
    return h;
}

Heightmap* Assets::heightmap(const std::string& path) {
    if (path.empty()) return nullptr;
    HeightmapFile& h = heightmapFile(path);
    return h.map.valid() ? &h.map : nullptr;
}

void Assets::createHeightmap(const std::string& path, Heightmap map) {
    HeightmapFile& h = m_heightmaps[path];
    h.map = std::move(map);
    h.error.clear();
    h.dirty = true;
    ++h.generation;
}

void Assets::heightmapEdited(const std::string& path, const SampleRect& rect) {
    HeightmapFile& h = m_heightmaps[path];
    h.dirty = true;
    u32 x0, z0, x1, z1;
    if (!chunksOf(h.map, rect, x0, z0, x1, z1)) return;
    const u32 n = terrainChunks(h.map);
    MeshData mesh;
    for (auto& [id, t] : m_terrains) {
        if (t.heightmap != path || t.generation != h.generation || t.view.chunks.size() != static_cast<size_t>(n) * n) continue;
        for (u32 z = z0; z <= z1; ++z)
            for (u32 x = x0; x <= x1; ++x) {
                buildTerrainChunk(h.map, t.size, x, z, 0, mesh);
                m_renderer->replaceMesh(t.view.chunks[static_cast<size_t>(z) * n + x], mesh);
            }
    }
}

bool Assets::hasUnsavedEdits() const {
    for (const auto& [path, h] : m_heightmaps)
        if (h.dirty) return true;
    for (const auto& [path, f] : m_scatters)
        if (f.entry.dirty) return true;
    return false;
}

bool Assets::saveEdits(std::string& error) {
    bool ok = true;
    for (auto& [path, h] : m_heightmaps) {
        if (!h.dirty || !h.map.valid()) continue;
        const std::vector<u8> png = encodeHeightmap(h.map);
        if (!writeBytes(resolve(path), std::string(png.begin(), png.end()))) {
            error += "cannot write " + path + "\n";
            ok = false;
            continue;
        }
        h.dirty = false;
        h.time = writeTime(resolve(path));
    }
    for (auto& [path, f] : m_scatters) {
        if (!f.entry.dirty) continue;
        if (!writeBytes(resolve(path), scatterText(f.entry.items))) {
            error += "cannot write " + path + "\n";
            ok = false;
            continue;
        }
        f.entry.dirty = false;
        f.time = writeTime(resolve(path));
    }
    return ok;
}

void Assets::discardEdits() {
    for (auto it = m_heightmaps.begin(); it != m_heightmaps.end();) it = it->second.dirty ? m_heightmaps.erase(it) : std::next(it);
    for (auto it = m_scatters.begin(); it != m_scatters.end();) it = it->second.entry.dirty ? m_scatters.erase(it) : std::next(it);
}

const TerrainView& Assets::terrain(u32 entityId, const std::string& heightmap, Vec3 size) {
    TerrainEntry& t = m_terrains[entityId];
    static const Heightmap kFlat = flatHeightmap(2, 0); // no heightmap yet: a flat box bottom
    const Heightmap* map = &kFlat;
    u32 generation = 0;
    std::string error;
    if (!heightmap.empty()) {
        HeightmapFile& h = heightmapFile(heightmap);
        map = h.map.valid() ? &h.map : nullptr;
        generation = h.generation;
        error = h.error;
    }
    const bool same = t.heightmap == heightmap && t.size.x == size.x && t.size.y == size.y && t.size.z == size.z && t.generation == generation;
    t.view.map = map;
    t.view.error = error;
    if (same) return t.view;
    t.heightmap = heightmap;
    t.size = size;
    t.generation = generation;
    if (!map) return t.view;
    const u32 n = terrainChunks(*map);
    MeshData mesh;
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
            buildTerrainChunk(*map, size, x, z, 0, mesh);
            const size_t i = static_cast<size_t>(z) * n + x;
            if (i < t.view.chunks.size()) m_renderer->replaceMesh(t.view.chunks[i], mesh);
            else t.view.chunks.push_back(m_renderer->createMesh(mesh));
        }
    // Fewer chunks than before (a smaller map): the extra meshes stay allocated but aren't drawn
    t.view.chunks.resize(static_cast<size_t>(n) * n);
    return t.view;
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

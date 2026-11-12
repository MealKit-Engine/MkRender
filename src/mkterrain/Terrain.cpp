#include "mkrender/Terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Static stb copies (PNG only), so they don't clash with a host's stb_image
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Moved from MealKit's modules/asset (Terrain.cpp, Scatter.cpp): same samples, triangles, brushes and file bytes.
namespace mkrender {
namespace {

constexpr f32 kMaxSample = 65535.0f;

struct Grid { // sample spacing of a placed heightmap
    Vec3 origin; // world position of sample (0, 0) at height 0
    f32 dx, dz, height;
};

Grid gridOf(const Heightmap& map, Vec3 center, Vec3 size) {
    const f32 cells = static_cast<f32>(map.size - 1);
    return {center - size * 0.5f, size.x / cells, size.z / cells, size.y};
}

f32 sampleMeters(const Heightmap& map, u32 x, u32 z, f32 height) { return static_cast<f32>(map.at(x, z)) * (height / kMaxSample); }

u32 crc32(const u8* data, size_t size) {
    static u32 table[256] = {};
    if (!table[1])
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void putU32(std::vector<u8>& out, u32 v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<u8>(v >> (i * 8)));
}

void chunk(std::vector<u8>& out, const char* type, const u8* data, size_t size) {
    putU32(out, static_cast<u32>(size));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (size) out.insert(out.end(), data, data + size);
    putU32(out, crc32(out.data() + start, out.size() - start));
}

// The engine's half float (asset/MeshData.cpp)
u16 toHalf(f32 f) {
    u32 x;
    std::memcpy(&x, &f, 4);
    const u32 sign = (x >> 16) & 0x8000u;
    const i32 exp = static_cast<i32>((x >> 23) & 0xFF) - 127 + 15;
    u32 mant = x & 0x7FFFFFu;
    if (exp <= 0) { // very small value: denormal half or 0
        if (exp < -10) return static_cast<u16>(sign);
        mant |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - exp);
        return static_cast<u16>(sign | ((mant + (1u << (shift - 1))) >> shift));
    }
    if (exp >= 31) return static_cast<u16>(sign | 0x7C00u); // infinity (NaN too)
    const u32 h = sign | (static_cast<u32>(exp) << 10) | (mant >> 13);
    return static_cast<u16>(h + ((mant >> 12) & 1u)); // rounding (correct even if the carry overflows into the exponent)
}
u32 packHalf2(f32 u, f32 v) { return static_cast<u32>(toHalf(u)) | (static_cast<u32>(toHalf(v)) << 16); }

} // namespace

Heightmap flatHeightmap(u32 size, u16 value) {
    Heightmap map;
    map.size = std::max(size, 2u);
    map.samples.assign(static_cast<size_t>(map.size) * map.size, value);
    return map;
}

bool decodeHeightmap(const u8* png, size_t bytes, Heightmap& out, std::string& error) {
    int w = 0, h = 0, channels = 0;
    stbi_us* pixels = stbi_load_16_from_memory(png, static_cast<int>(bytes), &w, &h, &channels, 1);
    if (!pixels) {
        error = std::string("not a readable PNG (") + stbi_failure_reason() + ")";
        return false;
    }
    if (w != h || w < 2) {
        stbi_image_free(pixels);
        error = "heightmap must be square with at least 2 samples per side (got " + std::to_string(w) + "x" +
                std::to_string(h) + ")";
        return false;
    }
    out.size = static_cast<u32>(w);
    out.samples.resize(static_cast<size_t>(w) * w);
    for (int row = 0; row < h; ++row) // image top row = +z edge
        std::memcpy(&out.samples[static_cast<size_t>(h - 1 - row) * w], pixels + static_cast<size_t>(row) * w,
                    sizeof(u16) * w);
    stbi_image_free(pixels);
    return true;
}

std::vector<u8> encodeHeightmap(const Heightmap& map) {
    const u32 n = map.size;
    std::vector<u8> raw; // filter byte 0 + big-endian 16-bit samples per row
    raw.reserve(static_cast<size_t>(n) * (n * 2 + 1));
    for (u32 row = 0; row < n; ++row) {
        raw.push_back(0);
        const u32 z = n - 1 - row;
        for (u32 x = 0; x < n; ++x) {
            const u16 s = map.at(x, z);
            raw.push_back(static_cast<u8>(s >> 8));
            raw.push_back(static_cast<u8>(s & 0xFF));
        }
    }
    int zlen = 0;
    unsigned char* zlib = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &zlen, 8);
    std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<u8> header;
    putU32(header, n);
    putU32(header, n);
    header.insert(header.end(), {16, 0, 0, 0, 0}); // 16-bit, grayscale, deflate, adaptive filters, no interlace
    chunk(out, "IHDR", header.data(), header.size());
    chunk(out, "IDAT", zlib, static_cast<size_t>(zlen));
    chunk(out, "IEND", nullptr, 0);
    STBIW_FREE(zlib);
    return out;
}

bool terrainHeightAt(const Heightmap& map, Vec3 center, Vec3 size, f32 x, f32 z, f32* y) {
    if (!map.valid()) return false;
    const Grid g = gridOf(map, center, size);
    const f32 fx = (x - g.origin.x) / g.dx, fz = (z - g.origin.z) / g.dz;
    const f32 cells = static_cast<f32>(map.size - 1);
    if (!(fx >= 0.0f && fz >= 0.0f && fx <= cells && fz <= cells)) return false;
    const u32 i = std::min(static_cast<u32>(fx), map.size - 2), j = std::min(static_cast<u32>(fz), map.size - 2);
    const f32 u = fx - static_cast<f32>(i), v = fz - static_cast<f32>(j);
    const f32 a = sampleMeters(map, i, j, g.height), b = sampleMeters(map, i + 1, j, g.height);
    const f32 c = sampleMeters(map, i + 1, j + 1, g.height), d = sampleMeters(map, i, j + 1, g.height);
    // Same diagonal as Jolt's height field: (i, j) - (i + 1, j + 1)
    const f32 h = v >= u ? a + (c - d) * u + (d - a) * v : a + (b - a) * u + (c - b) * v;
    *y = g.origin.y + h;
    return true;
}

f32 raycastTerrain(const Heightmap& map, Vec3 center, Vec3 size, Vec3 origin, Vec3 dir, f32 maxDistance) {
    if (!map.valid()) return -1.0f;
    const Vec3 lo = center - size * 0.5f, hi = center + size * 0.5f;
    f32 t0 = 0.0f, t1 = maxDistance; // clip to the box
    const f32 o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z}, l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(d[k]) < 1e-8f) {
            if (o[k] < l[k] || o[k] > h[k]) return -1.0f;
            continue;
        }
        f32 a = (l[k] - o[k]) / d[k], b = (h[k] - o[k]) / d[k];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return -1.0f;
    }
    const Grid g = gridOf(map, center, size);
    const f32 step = std::max(0.05f, std::min(g.dx, g.dz) * 0.5f);
    auto above = [&](f32 t, bool* inside) {
        const Vec3 p = origin + dir * t;
        f32 y = 0.0f;
        *inside = terrainHeightAt(map, center, size, p.x, p.z, &y);
        return p.y - y;
    };
    bool inside = false;
    f32 prevT = t0, prev = above(t0, &inside);
    if (inside && prev <= 0.0f) return t0;
    for (f32 t = t0 + step;; t += step) {
        const f32 tc = std::min(t, t1);
        const f32 cur = above(tc, &inside);
        if (inside && cur <= 0.0f) { // bisect between prevT and tc
            f32 a = prevT, b = tc;
            for (int k = 0; k < 16; ++k) {
                const f32 m = (a + b) * 0.5f;
                bool in = false;
                (above(m, &in) > 0.0f ? a : b) = m;
            }
            return b;
        }
        prevT = tc;
        prev = cur;
        if (tc >= t1) return -1.0f;
    }
}

u32 terrainChunks(const Heightmap& map) {
    return map.valid() ? (map.size - 1 + kTerrainChunkCells - 1) / kTerrainChunkCells : 0;
}

bool chunksOf(const Heightmap& map, const SampleRect& rect, u32& cx0, u32& cz0, u32& cx1, u32& cz1) {
    const u32 chunks = terrainChunks(map);
    if (rect.empty || chunks == 0) return false;
    const auto chunkOf = [&](u32 s) { return std::min(s / kTerrainChunkCells, chunks - 1); };
    cx0 = chunkOf(rect.x0 > 0 ? rect.x0 - 1 : 0);
    cx1 = chunkOf(rect.x1 + 1);
    cz0 = chunkOf(rect.z0 > 0 ? rect.z0 - 1 : 0);
    cz1 = chunkOf(rect.z1 + 1);
    return true;
}

void buildTerrainChunk(const Heightmap& map, Vec3 size, u32 cx, u32 cz, u32 lod, MeshData& out) {
    out.vertices.clear();
    out.indices.clear();
    out.skin.clear();
    if (!map.valid()) return;
    const u32 cells = map.size - 1, step = 1u << std::min(lod, kTerrainLods - 1);
    auto axis = [&](u32 c, std::vector<u32>& list) {
        const u32 a = c * kTerrainChunkCells, b = std::min(a + kTerrainChunkCells, cells);
        list.clear();
        for (u32 i = a; i < b; i += step) list.push_back(i);
        list.push_back(b);
    };
    thread_local std::vector<u32> xs, zs;
    axis(cx, xs);
    axis(cz, zs);
    const Grid g = gridOf(map, {}, size);
    const f32 uvScale = 1.0f / static_cast<f32>(cells);
    auto h = [&](u32 x, u32 z) { return sampleMeters(map, x, z, g.height); };
    auto vertex = [&](u32 x, u32 z, f32 drop) {
        const u32 xl = x > 0 ? x - 1 : x, xr = std::min(x + 1, cells), zd = z > 0 ? z - 1 : z, zu = std::min(z + 1, cells);
        const Vec3 n = normalize(Vec3{-(h(xr, z) - h(xl, z)) / (static_cast<f32>(xr - xl) * g.dx), 1.0f,
                                  -(h(x, zu) - h(x, zd)) / (static_cast<f32>(zu - zd) * g.dz)});
        Vertex v;
        v.position = {g.origin.x + static_cast<f32>(x) * g.dx, g.origin.y + h(x, z) - drop, g.origin.z + static_cast<f32>(z) * g.dz};
        v.normal = n;
        v.uv = packHalf2(static_cast<f32>(x) * uvScale, static_cast<f32>(z) * uvScale);
        out.vertices.push_back(v);
    };
    const u32 w = static_cast<u32>(xs.size());
    for (u32 z : zs)
        for (u32 x : xs) vertex(x, z, 0.0f);
    for (u32 j = 0; j + 1 < zs.size(); ++j)
        for (u32 i = 0; i + 1 < w; ++i) {
            const u32 a = j * w + i, b = a + 1, d = a + w, c = d + 1;
            out.indices.insert(out.indices.end(), {a, d, c, a, c, b}); // front face clockwise from above (makeBox)
        }
    // Skirts: each edge copied downward, both windings (seen from either side)
    const f32 drop = g.height * 0.02f + std::max(g.dx, g.dz) * static_cast<f32>(step);
    auto skirt = [&](auto index, u32 count) {
        const u32 base = static_cast<u32>(out.vertices.size());
        for (u32 k = 0; k < count; ++k) {
            Vertex v = out.vertices[index(k)];
            v.position.y -= drop;
            out.vertices.push_back(v);
        }
        for (u32 k = 0; k + 1 < count; ++k) {
            const u32 a = index(k), b = index(k + 1), a2 = base + k, b2 = base + k + 1;
            out.indices.insert(out.indices.end(), {a, b, b2, a, b2, a2, a, b2, b, a, a2, b2});
        }
    };
    const u32 rows = static_cast<u32>(zs.size());
    skirt([&](u32 k) { return k; }, w);
    skirt([&](u32 k) { return (rows - 1) * w + k; }, w);
    skirt([&](u32 k) { return k * w; }, rows);
    skirt([&](u32 k) { return k * w + w - 1; }, rows);
}

SampleRect applyBrush(Heightmap& map, Vec3 center, Vec3 size, TerrainBrush brush, Vec3 point, f32 radius, f32 amount,
                      u16 flattenTo) {
    SampleRect rect;
    if (!map.valid() || radius <= 0.0f) return rect;
    const Grid g = gridOf(map, center, size);
    const i32 last = static_cast<i32>(map.size - 1);
    const i32 x0 = std::clamp(static_cast<i32>(std::floor((point.x - radius - g.origin.x) / g.dx)), 0, last);
    const i32 x1 = std::clamp(static_cast<i32>(std::ceil((point.x + radius - g.origin.x) / g.dx)), 0, last);
    const i32 z0 = std::clamp(static_cast<i32>(std::floor((point.z - radius - g.origin.z) / g.dz)), 0, last);
    const i32 z1 = std::clamp(static_cast<i32>(std::ceil((point.z + radius - g.origin.z) / g.dz)), 0, last);
    if (x0 > x1 || z0 > z1) return rect;
    thread_local std::vector<u16> source;
    if (brush == TerrainBrush::Smooth) source = map.samples;
    bool touched = false;
    for (i32 z = z0; z <= z1; ++z)
        for (i32 x = x0; x <= x1; ++x) {
            const f32 px = g.origin.x + static_cast<f32>(x) * g.dx - point.x, pz = g.origin.z + static_cast<f32>(z) * g.dz - point.z;
            const f32 dist = std::sqrt(px * px + pz * pz);
            if (dist >= radius) continue;
            const f32 t = 1.0f - dist / radius, weight = t * t * (3.0f - 2.0f * t);
            u16& s = map.samples[static_cast<size_t>(z) * map.size + x];
            f32 value = static_cast<f32>(s);
            switch (brush) {
            case TerrainBrush::Raise: value += amount * weight * kMaxSample; break;
            case TerrainBrush::Lower: value -= amount * weight * kMaxSample; break;
            case TerrainBrush::Flatten: value = lerp(value, static_cast<f32>(flattenTo), std::min(1.0f, amount * weight)); break;
            case TerrainBrush::Smooth: {
                f32 sum = 0.0f;
                u32 count = 0;
                for (i32 dz = -1; dz <= 1; ++dz)
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const i32 sx = x + dx, sz = z + dz;
                        if (sx < 0 || sz < 0 || sx > last || sz > last) continue;
                        sum += static_cast<f32>(source[static_cast<size_t>(sz) * map.size + sx]);
                        ++count;
                    }
                value = lerp(value, sum / static_cast<f32>(count), std::min(1.0f, amount * weight));
                break;
            }
            }
            const u16 next = static_cast<u16>(std::lround(std::clamp(value, 0.0f, kMaxSample)));
            if (next == s) continue;
            s = next;
            touched = true;
        }
    if (touched) rect = {static_cast<u32>(x0), static_cast<u32>(z0), static_cast<u32>(x1), static_cast<u32>(z1), false};
    return rect;
}

bool readScatter(const std::string& text, std::vector<ScatterInstance>& out, std::string& error) {
    out.clear();
    bool format = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (!format) {
            if (line.rfind("@format scatter ", 0) != 0) {
                error = "not a scatter file";
                return false;
            }
            format = true;
            continue;
        }
        ScatterInstance s;
        if (std::sscanf(line.c_str(), "i %f %f %f %f %f", &s.position.x, &s.position.y, &s.position.z, &s.yaw, &s.scale) == 5) out.push_back(s);
    }
    return true;
}

std::string scatterText(const std::vector<ScatterInstance>& items) {
    std::string out = "# MealKit scatter layer: i x y z yaw scale (painted in the editor, view only)\n@format scatter 1\n";
    char line[128];
    for (const ScatterInstance& s : items) {
        std::snprintf(line, sizeof(line), "i %.3f %.3f %.3f %.3f %.3f\n", s.position.x, s.position.y, s.position.z, s.yaw, s.scale);
        out += line;
    }
    return out;
}

u32 eraseScatter(std::vector<ScatterInstance>& items, Vec3 center, f32 radius) {
    const size_t before = items.size();
    std::erase_if(items, [&](const ScatterInstance& s) {
        const f32 dx = s.position.x - center.x, dz = s.position.z - center.z;
        return dx * dx + dz * dz < radius * radius;
    });
    return static_cast<u32>(before - items.size());
}

} // namespace mkrender

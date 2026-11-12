#pragma once

#include "mkrender/Config.h"
#include "mkrender/Math.h"
#include "mkrender/Mesh.h"

#include <cmath>
#include <string>
#include <vector>

// mkterrain: heightmap terrain and scatter layers as plain data, the editing half of MealKit's asset/Terrain.h and asset/Scatter.h
// (same sample layout, triangles, brushes and file formats), used by the MkRender scene editor. No GPU, no files: the host reads and writes bytes.
//
// Terrain: 16-bit PNG heights filling an object's box. Sample (x, z) sits at
//   center + (-size.x/2 + x * size.x/(n-1), -size.y/2 + s/65535 * size.y, -size.z/2 + z * size.z/(n-1)), z = 0 at the -z edge (the image's top row is +z).
// Scatter: view-only copies of one model, "@format scatter 1" text with one "i x y z yaw scale" line per instance (world positions).
namespace mkrender {

// Samples in grid order: index = z * size + x
struct Heightmap {
    u32 size = 0; // samples per side
    std::vector<u16> samples;

    bool valid() const { return size >= 2 && samples.size() == static_cast<size_t>(size) * size; }
    u16 at(u32 x, u32 z) const { return samples[static_cast<size_t>(z) * size + x]; }
};

Heightmap flatHeightmap(u32 size, u16 value);
// PNG (16-bit or 8-bit, first channel) -> heightmap. Must be square
bool decodeHeightmap(const u8* png, size_t bytes, Heightmap& out, std::string& error);
// 16-bit grayscale PNG (deflate), the engine's encoder
std::vector<u8> encodeHeightmap(const Heightmap& map);

// Surface height (world y) at world x/z on the collision triangles (diagonal (i, j)-(i+1, j+1) like Jolt). false outside
bool terrainHeightAt(const Heightmap& map, Vec3 center, Vec3 size, f32 x, f32 z, f32* y);
// First hit of a ray (dir normalized): distance, or -1 within maxDistance
f32 raycastTerrain(const Heightmap& map, Vec3 center, Vec3 size, Vec3 origin, Vec3 dir, f32 maxDistance);

// Chunk meshes (vertices relative to the box center, skirts hide cracks), the engine's view meshes
inline constexpr u32 kTerrainChunkCells = 32;
inline constexpr u32 kTerrainLods = 3;
u32 terrainChunks(const Heightmap& map); // per side
void buildTerrainChunk(const Heightmap& map, Vec3 size, u32 cx, u32 cz, u32 lod, MeshData& out);

// Brushes
enum class TerrainBrush : u8 { Raise, Lower, Flatten, Smooth };
struct SampleRect {
    u32 x0 = 0, z0 = 0, x1 = 0, z1 = 0; // inclusive
    bool empty = true;
};
// At a world point (x/z) with radius (m) and smooth falloff. amount: fraction of the box height (Raise/Lower) or blend 0..1 (Flatten/Smooth),
// already times the frame time. Flatten moves toward flattenTo (sample value). Returns the touched samples
SampleRect applyBrush(Heightmap& map, Vec3 center, Vec3 size, TerrainBrush brush, Vec3 point, f32 radius, f32 amount, u16 flattenTo);
// Chunks a change touches (a sample is shared by neighbour chunks and changes their normals): inclusive chunk range, false if none
bool chunksOf(const Heightmap& map, const SampleRect& rect, u32& cx0, u32& cz0, u32& cx1, u32& cz1);

// ---------------------------------------------------------------- Scatter
struct ScatterInstance {
    Vec3 position; // world
    f32 yaw = 0.0f;   // radians around y
    f32 scale = 1.0f;
};
bool readScatter(const std::string& text, std::vector<ScatterInstance>& out, std::string& error);
std::string scatterText(const std::vector<ScatterInstance>& items);

struct ScatterPaint {
    Vec3 center;
    f32 radius = 4.0f;
    f32 density = 0.5f; // per m²
    f32 minScale = 0.8f, maxScale = 1.2f;
    bool randomYaw = true;
};
// Adds up to maxAdd instances at random points in the circle where floorAt(x, z, &y) finds ground, keeping spacing
// 0.75 / sqrt(density). Returns the number added
template <class FloorAt, class Random>
u32 paintScatter(std::vector<ScatterInstance>& items, const ScatterPaint& p, u32 maxAdd, FloorAt&& floorAt, Random&& random01) {
    if (p.density <= 0.0f || p.radius <= 0.0f) return 0;
    const f32 spacing = 0.75f / std::sqrt(p.density), spacing2 = spacing * spacing;
    const f32 reach = (p.radius + spacing) * (p.radius + spacing);
    std::vector<u32> near;
    for (u32 i = 0; i < items.size(); ++i) {
        const f32 dx = items[i].position.x - p.center.x, dz = items[i].position.z - p.center.z;
        if (dx * dx + dz * dz <= reach) near.push_back(i);
    }
    u32 added = 0;
    for (u32 attempt = 0; attempt < maxAdd * 4 && added < maxAdd; ++attempt) {
        const f32 a = random01() * 6.2831853f, r = p.radius * std::sqrt(random01());
        const f32 x = p.center.x + std::cos(a) * r, z = p.center.z + std::sin(a) * r;
        bool crowded = false;
        for (u32 i : near) {
            const f32 dx = items[i].position.x - x, dz = items[i].position.z - z;
            if (dx * dx + dz * dz < spacing2) {
                crowded = true;
                break;
            }
        }
        f32 y = 0.0f;
        if (crowded || !floorAt(x, z, &y)) continue;
        ScatterInstance s;
        s.position = {x, y, z};
        s.yaw = p.randomYaw ? random01() * 6.2831853f : 0.0f;
        s.scale = p.minScale + (p.maxScale - p.minScale) * random01();
        near.push_back(static_cast<u32>(items.size()));
        items.push_back(s);
        ++added;
    }
    return added;
}
u32 eraseScatter(std::vector<ScatterInstance>& items, Vec3 center, f32 radius); // removed count

} // namespace mkrender

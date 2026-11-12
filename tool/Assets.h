#pragma once

#include "mkrender/Bake.h"
#include "mkrender/Renderer.h"

#include <map>
#include <string>
#include <vector>

// Renderer resources for a project's assets: glTF models by path (relative to assets/), the unit box drawn for objects
// without a model, and terrain meshes from heightmaps. Uploaded on first use and kept (the renderer has no deletion;
// reloading a changed file uploads it again).
namespace mkrender::tool {

struct ModelEntry {
    MeshId mesh = kNoMesh;
    Vec3 boundsMin{-0.5f, -0.5f, -0.5f}, boundsMax{0.5f, 0.5f, 0.5f}; // model space
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    u32 triangles = 0, materials = 0, textures = 0;
};

// One sprite image cell as a 1 x 1 m quad on x/y facing -z (pivot at the origin), alpha-tested texture
struct SpriteEntry {
    MeshId mesh = kNoMesh;
    u32 cellWidth = 0, cellHeight = 0; // pixels
    std::string error;
};

// Scatter layer instances (assets/terrain/<scene>_<id>.scatter, "i x y z yaw scale" lines, world positions)
struct ScatterItem {
    Vec3 position;
    f32 yaw = 0.0f, scale = 1.0f;
};
struct ScatterEntry {
    std::vector<ScatterItem> items;
    std::string error;
};

struct TextureImage;

// Sky HDRI, LUT and reflection probe uploads (index 0 = none, error says why)
struct GpuEntry {
    u32 id = 0;
    std::string error;
};

class Assets {
public:
    void init(Renderer& renderer);
    void setAssetsDir(const std::string& dir); // forgets loaded models (another project)
    const std::string& assetsDir() const { return m_assetsDir; }
    std::string resolve(const std::string& path) const; // relative to assets, or absolute as is
    std::string relative(const std::string& absolute) const; // inside assets: relative with '/', else the absolute path

    const ModelEntry& model(const std::string& path); // loads on first use
    void reload(const std::string& path);
    void reloadAll();
    MeshId box() const { return m_box; }
    // Terrain mesh of one object (rebuilt when its heightmap or size changes). Vertices relative to the box center
    MeshId terrain(u32 entityId, const std::string& heightmap, Vec3 size, std::string* error);

    // Sprite quad of one cell of an image sheet (cached per image and cell layout)
    const SpriteEntry& sprite(const std::string& image, int columns, int rows, int cell, f32 pivotX, f32 pivotY, bool flipX, bool flipY);
    // Scatter file (reread when it changes on disk)
    const ScatterEntry& scatter(const std::string& file);

    // Sky HDRI: the engine's environment cache (<project>/build/cache/environments/<hash>.mkenv, mkbake) or baked here and written there
    const GpuEntry& environment(const std::string& hdri);
    // Color grading LUT (.cube), reread when it changes
    const GpuEntry& lut(const std::string& cube);
    // Reflection probe file (.mkprobe), reread when it changes (a bake writes it again)
    const GpuEntry& probe(const std::string& file);
    // Model textures as BC7/BC5 from the engine's texture cache (<project>/build/cache/textures/<hash>.mktex), compressed here when missing.
    // Off: RGBA8 with mips (faster first load of a big project). Takes effect on Reload models
    void setCompressTextures(bool on) { m_compress = on; }
    bool compressTextures() const { return m_compress; }
    std::string cacheDir() const; // <project>/build/cache (empty without a project)

    // Model files under assets (relative paths, sorted). Scanned on first call and by rescan()
    const std::vector<std::string>& modelFiles();
    std::vector<std::string> files(const char* extension) const; // e.g. ".scene"
    void rescan() { m_scanned = false; }

private:
    struct TerrainEntry {
        std::string heightmap;
        Vec3 size;
        MeshId mesh = kNoMesh;
        std::string error;
    };
    Renderer* m_renderer = nullptr;
    std::string m_assetsDir;
    std::map<std::string, ModelEntry> m_models;
    std::map<u32, TerrainEntry> m_terrains;
    struct ImageEntry {
        u32 material = 0, width = 0, height = 0;
        std::string error;
    };
    std::map<std::string, ImageEntry> m_images; // sprite textures by path
    std::map<std::string, SpriteEntry> m_sprites;
    struct ScatterFile {
        ScatterEntry entry;
        long long time = 0;
    };
    std::map<std::string, ScatterFile> m_scatters;
    struct TimedEntry {
        GpuEntry entry;
        long long time = 0;
    };
    std::map<std::string, TimedEntry> m_environments, m_luts, m_probes;
    bool m_compress = true;
    u32 uploadTexture(const TextureImage& image);
    MeshId m_box = kNoMesh;
    std::vector<std::string> m_modelFiles;
    bool m_scanned = false;
};

} // namespace mkrender::tool

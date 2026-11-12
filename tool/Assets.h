#pragma once

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
    MeshId m_box = kNoMesh;
    std::vector<std::string> m_modelFiles;
    bool m_scanned = false;
};

} // namespace mkrender::tool

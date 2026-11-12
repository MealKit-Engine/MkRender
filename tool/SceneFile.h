#pragma once

#include "mkrender/Math.h"

#include <string>
#include <utility>
#include <vector>

// A MealKit project's .scene file (text format "scene" 5), read and written the way the engine's scene module does
// (modules/scene Scene.cpp: same keys, order, float text and field lines), so the engine and the editor read what this writes.
// Field values of object kinds stay text: game kinds' fields survive without the game's code.
namespace mkrender::tool {

inline constexpr unsigned kSceneFormatVersion = 5;

inline constexpr const char* kKindSolid = "solid";
inline constexpr const char* kKindDecoration = "decoration";
inline constexpr const char* kKindSpawn = "spawn";
inline constexpr const char* kKindProbe = "reflection_probe";
inline constexpr const char* kKindPointLight = "point_light";
inline constexpr const char* kKindCamera = "virtual_camera";
inline constexpr const char* kKindScatter = "scatter";
inline constexpr const char* kKindReverb = "reverb_zone";
inline constexpr const char* kKindTerrain = "terrain";
inline constexpr const char* kKindSprite = "sprite";

using TextFields = std::vector<std::pair<std::string, std::string>>; // (path, value text) in file order

struct Entity {
    u32 id = 0;
    u32 parent = 0; // 0 = top level; position/rotation/scale are parent-relative
    std::string name;
    std::string kind = kKindSolid;
    Vec3 position;
    Quat rotation;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    u32 color = 0xFFC8C8C8; // RGBA8 sRGB (mkrender::rgba layout)
    u32 flags = 0;          // mkrender kInstance* bits
    std::string folder;     // scene tree folder ("Props/Trees"), editor only
    std::string model;      // glTF relative to assets; a box if empty
    TextFields fields;

    bool is(const char* k) const { return kind == k; }
    std::string field(const std::string& key) const;
    void setField(const std::string& key, const std::string& value);
};

struct Environment {
    Vec3 sunDirection{0.4f, 0.8f, 0.3f};
    Vec3 sunColor{1.0f, 0.95f, 0.85f};
    Vec3 skyColor{0.18f, 0.36f, 0.85f};
    Vec3 groundColor{0.20f, 0.17f, 0.14f};
    Vec3 fogColor{0.55f, 0.70f, 0.95f};
    f32 fogDensity = 0.003f;
    std::string hdri;
    f32 hdriIntensity = 1.0f;
    f32 hdriRotation = 0.0f; // degrees
    TextFields post;         // "post <field> <value>" lines, every PostFields field in declaration order
    std::string postValue(const std::string& key) const;
    void setPost(const std::string& key, const std::string& value);
};

struct Scene {
    std::vector<Entity> entities;
    Environment environment;
    u32 nextId = 1;

    Entity& add(const std::string& kind, const std::string& name);
    Entity* find(u32 id);
    const Entity* find(u32 id) const;
    int indexOf(u32 id) const;
};

struct Transform {
    Vec3 position;
    Quat rotation;
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

Quat mul(Quat a, Quat b); // a then... same as the engine's a * b (b applied first)
Quat normalizeQuat(Quat q);
Transform worldTransform(const Scene& scene, const Entity& e);
void setWorldTransform(const Scene& scene, Entity& e, const Transform& world);
bool isDescendant(const Scene& scene, u32 id, u32 ancestor);
std::vector<u32> subtree(const Scene& scene, u32 id);

// Text of the file (also the undo snapshot and the "edited" check)
std::string writeScene(const Scene& scene);
bool readScene(const std::string& text, Scene& out, std::string& error, std::vector<std::string>* warnings);
bool loadScene(const std::string& path, Scene& out, std::string& error, std::vector<std::string>* warnings);
bool saveScene(const std::string& path, const Scene& scene, std::string& error);

// Prefabs (assets/prefabs/*.prefab, "@format prefab 1": entity blocks only), like the engine's scene::savePrefab/placePrefab:
// roots (with children) in world coordinates relative to the first root's x/z, ids renumbered from 1; placing adds copies with new ids
std::string writePrefab(const Scene& scene, const std::vector<u32>& roots);
bool loadPrefab(const std::string& path, Scene& out, std::string& error);
std::vector<u32> placePrefab(Scene& scene, const Scene& prefab, const Transform& at); // new root ids
Transform combineTransforms(const Transform& parent, const Transform& local);

std::string floatText(f32 v);           // shortest text that reads back to the same float, -0 as 0 (the engine's text::writeFloats)
std::string floatsText(const f32* v, int count);
// "#RRGGBB" / "#RRGGBBAA" (reflect Color fields) <-> linear-free sRGB bytes
bool parseColorText(const std::string& text, u8 rgba[4]);
std::string colorText(const u8 rgba[4]);

// One field of a kind from the object kinds file (MealKit-Contract "Object kinds")
struct KindField {
    std::string path;    // scene field key ("a.b", "a[2]")
    std::string type;    // bool int uint float enum vec3 quat string seconds color row id text
    std::string value;   // default text
    std::string label;   // display name without hints
    std::string hint;    // "gizmo=radius" parts after '|'
    std::vector<std::string> enumNames;
    bool view = false, readOnly = false;
};

// Object kinds: the built-in ones (the engine's scene::builtinTypes), replaced by the project's build/editor/kinds.txt when it exists
struct KindInfo {
    std::string name;
    std::string label;
    bool marker = false;   // a point in space (spawn, light, camera): drawn as an icon here
    bool area = false;     // a box volume (probe, reverb zone): drawn as lines here
    bool cosmetic = false; // no effect on game results
    bool game = false;     // declared by the game (MEALKIT_OBJECT)
    std::vector<KindField> fields; // empty without the kinds file
};
const std::vector<KindInfo>& kinds();
const KindInfo* findKind(const std::string& name);
const KindField* findKindField(const std::string& kind, const std::string& path);
// Reads an "object-kinds 1" file and uses its kinds (false: kinds unchanged). resetKinds goes back to the built-in list
bool loadKinds(const std::string& path, std::string& error);
void resetKinds();
// Default field lines of a new object of the kind (from the kinds file, else the built-in defaults; empty for kinds without fields)
TextFields defaultFields(const std::string& kind);

} // namespace mkrender::tool

#include "SceneFile.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace mkrender::tool {
namespace {

std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

bool readFloats(const char* text, f32* out, int count) {
    const char* p = text;
    for (int i = 0; i < count; ++i) {
        char* end = nullptr;
        out[i] = std::strtof(p, &end);
        if (end == p) return false;
        p = end;
    }
    return true;
}

bool readVec3(const char* text, Vec3& v) {
    f32 f[3];
    if (!readFloats(text, f, 3)) return false;
    v = {f[0], f[1], f[2]};
    return true;
}

bool validKindName(const char* text) {
    if (!std::isalpha(static_cast<unsigned char>(*text))) return false;
    for (const char* p = text; *p; ++p)
        if (!std::isalnum(static_cast<unsigned char>(*p)) && *p != '_') return false;
    return true;
}

// PostFields in declaration order with their default text (modules/scene Scene.h)
const TextFields& postDefaults() {
    static const TextFields d = {{"toneMapping", "true"}, {"punchy", "true"},      {"exposure", "0"},   {"lut", ""},
                                 {"bloomIntensity", "0.25"}, {"bloomThreshold", "1"}, {"bloomTint", "1 1 1"},
                                 {"vignette", "0"},      {"vignetteSmoothness", "0.5"}, {"grain", "0"}, {"chromaticAberration", "0"}};
    return d;
}

Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
f32 safeDiv(f32 a, f32 b) { return b > 1e-6f || b < -1e-6f ? a / b : a; }

Transform combine(const Transform& parent, const Transform& local) {
    const Vec3 scaled{local.position.x * parent.scale.x, local.position.y * parent.scale.y, local.position.z * parent.scale.z};
    return {parent.position + rotate(parent.rotation, scaled), normalizeQuat(mul(parent.rotation, local.rotation)),
            {parent.scale.x * local.scale.x, parent.scale.y * local.scale.y, parent.scale.z * local.scale.z}};
}

void line(std::string& out, const char* key, const std::string& value) {
    out += key;
    out += ' ';
    out += value;
    out += '\n';
}

} // namespace

std::string Entity::field(const std::string& key) const {
    for (const auto& [k, v] : fields)
        if (k == key) return v;
    return {};
}

void Entity::setField(const std::string& key, const std::string& value) {
    for (auto& [k, v] : fields)
        if (k == key) {
            v = value;
            return;
        }
    fields.emplace_back(key, value);
}

std::string Environment::postValue(const std::string& key) const {
    for (const auto& [k, v] : post)
        if (k == key) return v;
    for (const auto& [k, v] : postDefaults())
        if (k == key) return v;
    return {};
}

void Environment::setPost(const std::string& key, const std::string& value) {
    for (auto& [k, v] : post)
        if (k == key) {
            v = value;
            return;
        }
    post.emplace_back(key, value);
}

Entity& Scene::add(const std::string& kind, const std::string& name) {
    Entity e;
    e.id = nextId++;
    e.kind = kind;
    e.name = name;
    entities.push_back(e);
    return entities.back();
}

int Scene::indexOf(u32 id) const {
    for (size_t i = 0; i < entities.size(); ++i)
        if (entities[i].id == id) return static_cast<int>(i);
    return -1;
}

Entity* Scene::find(u32 id) {
    const int i = indexOf(id);
    return i >= 0 ? &entities[static_cast<size_t>(i)] : nullptr;
}

const Entity* Scene::find(u32 id) const {
    const int i = indexOf(id);
    return i >= 0 ? &entities[static_cast<size_t>(i)] : nullptr;
}

Quat mul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat normalizeQuat(Quat q) {
    const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return len > 1e-12f ? Quat{q.x / len, q.y / len, q.z / len, q.w / len} : Quat{};
}

Transform worldTransform(const Scene& scene, const Entity& e) {
    Transform t{e.position, e.rotation, e.scale};
    u32 parentId = e.parent;
    for (size_t depth = 0; parentId && depth <= scene.entities.size(); ++depth) {
        const Entity* parent = scene.find(parentId);
        if (!parent) break;
        t = combine({parent->position, parent->rotation, parent->scale}, t);
        parentId = parent->parent;
    }
    return t;
}

void setWorldTransform(const Scene& scene, Entity& e, const Transform& world) {
    const Entity* parent = e.parent ? scene.find(e.parent) : nullptr;
    if (!parent) {
        e.position = world.position;
        e.rotation = world.rotation;
        e.scale = world.scale;
        return;
    }
    const Transform p = worldTransform(scene, *parent);
    const Quat inv = conjugate(p.rotation);
    const Vec3 rel = rotate(inv, world.position - p.position);
    e.position = {safeDiv(rel.x, p.scale.x), safeDiv(rel.y, p.scale.y), safeDiv(rel.z, p.scale.z)};
    e.rotation = normalizeQuat(mul(inv, world.rotation));
    e.scale = {safeDiv(world.scale.x, p.scale.x), safeDiv(world.scale.y, p.scale.y), safeDiv(world.scale.z, p.scale.z)};
}

bool isDescendant(const Scene& scene, u32 id, u32 ancestor) {
    for (size_t depth = 0; id && depth <= scene.entities.size(); ++depth) {
        if (id == ancestor) return true;
        const Entity* e = scene.find(id);
        if (!e) return false;
        id = e->parent;
    }
    return false;
}

std::vector<u32> subtree(const Scene& scene, u32 id) {
    std::vector<u32> ids;
    for (const Entity& e : scene.entities)
        if (isDescendant(scene, e.id, id)) ids.push_back(e.id);
    return ids;
}

std::string floatText(f32 v) {
    if (v == 0.0f) v = 0.0f; // -0 becomes 0
    char buf[32];
    const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

std::string floatsText(const f32* v, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        if (i) out += ' ';
        out += floatText(v[i]);
    }
    return out;
}

bool parseColorText(const std::string& text, u8 rgba[4]) {
    if (text.size() != 7 && text.size() != 9) return false;
    if (text[0] != '#') return false;
    const unsigned long v = std::strtoul(text.c_str() + 1, nullptr, 16);
    if (text.size() == 7) {
        rgba[0] = static_cast<u8>(v >> 16);
        rgba[1] = static_cast<u8>(v >> 8);
        rgba[2] = static_cast<u8>(v);
        rgba[3] = 255;
    } else {
        rgba[0] = static_cast<u8>(v >> 24);
        rgba[1] = static_cast<u8>(v >> 16);
        rgba[2] = static_cast<u8>(v >> 8);
        rgba[3] = static_cast<u8>(v);
    }
    return true;
}

std::string colorText(const u8 c[4]) {
    char buf[16];
    if (c[3] == 255) std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c[0], c[1], c[2]);
    else std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", c[0], c[1], c[2], c[3]);
    return buf;
}

std::string writeScene(const Scene& scene) {
    const Environment& env = scene.environment;
    std::string out = "# MealKit scene\n@format scene " + std::to_string(kSceneFormatVersion) + "\n\n";
    line(out, "sun", floatsText(&env.sunDirection.x, 3));
    line(out, "sunColor", floatsText(&env.sunColor.x, 3));
    line(out, "sky", floatsText(&env.skyColor.x, 3));
    line(out, "groundColor", floatsText(&env.groundColor.x, 3));
    const f32 fog[4] = {env.fogColor.x, env.fogColor.y, env.fogColor.z, env.fogDensity};
    line(out, "fog", floatsText(fog, 4));
    if (!env.hdri.empty()) {
        line(out, "hdri", env.hdri);
        line(out, "hdriIntensity", floatText(env.hdriIntensity));
        line(out, "hdriRotation", floatText(env.hdriRotation));
    }
    for (const auto& [key, value] : postDefaults()) out += "post " + key + " " + env.postValue(key) + "\n";
    for (const Entity& e : scene.entities) {
        out += "\nentity\nid " + std::to_string(e.id) + "\nname " + e.name + "\nkind " + e.kind + "\n";
        if (e.parent) out += "parent " + std::to_string(e.parent) + "\n";
        if (!e.folder.empty()) out += "folder " + e.folder + "\n";
        line(out, "position", floatsText(&e.position.x, 3));
        line(out, "rotation", floatsText(&e.rotation.x, 4));
        line(out, "scale", floatsText(&e.scale.x, 3));
        if (!e.model.empty()) line(out, "model", e.model);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "color %08X\nflags %u\n", e.color, e.flags);
        out += buf;
        for (const auto& [key, value] : e.fields) out += "field " + key + " " + value + "\n";
        out += "end\n";
    }
    return out;
}

bool readScene(const std::string& text, Scene& out, std::string& error, std::vector<std::string>* warnings) {
    Scene scene;
    Entity e;
    bool inEntity = false;
    int lineNo = 0;
    std::istringstream in(text);
    std::string raw;
    const auto warn = [&](const std::string& w) {
        if (warnings) warnings->push_back("line " + std::to_string(lineNo) + ": " + w);
    };
    while (std::getline(in, raw)) {
        ++lineNo;
        while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' ')) raw.pop_back();
        size_t start = raw.find_first_not_of(" \t");
        if (start == std::string::npos || raw[start] == '#') continue;
        std::string lineText = raw.substr(start);
        if (lineText[0] == '@') {
            unsigned version = 0;
            char kind[32] = {};
            if (std::sscanf(lineText.c_str(), "@format %31s %u", kind, &version) != 2 || std::strcmp(kind, "scene") != 0) {
                error = "line " + std::to_string(lineNo) + ": not a scene file";
                return false;
            }
            if (version > kSceneFormatVersion) warn("written in scene format " + std::to_string(version) + ", unknown keys are dropped");
            continue;
        }
        const size_t space = lineText.find(' ');
        const std::string key = lineText.substr(0, space);
        const std::string rest = space == std::string::npos ? std::string() : lineText.substr(space + 1);
        bool ok = true;
        if (key == "version") ok = true;
        else if (key == "entity") {
            ok = !inEntity;
            e = Entity{};
            inEntity = true;
        } else if (key == "end") {
            ok = inEntity;
            scene.entities.push_back(e);
            inEntity = false;
        } else if (inEntity) {
            if (key == "id") e.id = static_cast<u32>(std::strtoul(rest.c_str(), nullptr, 10));
            else if (key == "parent") e.parent = static_cast<u32>(std::strtoul(rest.c_str(), nullptr, 10));
            else if (key == "folder") e.folder = rest;
            else if (key == "name") e.name = rest;
            else if (key == "kind") {
                ok = validKindName(rest.c_str());
                e.kind = rest;
            } else if (key == "field") {
                const size_t sp = rest.find(' ');
                const std::string path = rest.substr(0, sp);
                ok = !path.empty();
                e.fields.emplace_back(path, sp == std::string::npos ? std::string() : rest.substr(sp + 1));
            } else if (key == "position") ok = readVec3(rest.c_str(), e.position);
            else if (key == "rotation") {
                f32 q[4];
                ok = readFloats(rest.c_str(), q, 4);
                e.rotation = {q[0], q[1], q[2], q[3]};
            } else if (key == "scale") ok = readVec3(rest.c_str(), e.scale);
            else if (key == "color") e.color = static_cast<u32>(std::strtoul(rest.c_str(), nullptr, 16));
            else if (key == "flags") e.flags = static_cast<u32>(std::strtoul(rest.c_str(), nullptr, 10));
            else if (key == "model") e.model = rest;
            else warn("unknown entity key '" + key + "' dropped");
        } else {
            Environment& env = scene.environment;
            if (key == "sun") ok = readVec3(rest.c_str(), env.sunDirection);
            else if (key == "sunColor") ok = readVec3(rest.c_str(), env.sunColor);
            else if (key == "sky") ok = readVec3(rest.c_str(), env.skyColor);
            else if (key == "groundColor") ok = readVec3(rest.c_str(), env.groundColor);
            else if (key == "fog") {
                f32 v[4];
                ok = readFloats(rest.c_str(), v, 4);
                env.fogColor = {v[0], v[1], v[2]};
                env.fogDensity = v[3];
            } else if (key == "hdri") env.hdri = rest;
            else if (key == "hdriIntensity") ok = readFloats(rest.c_str(), &env.hdriIntensity, 1);
            else if (key == "hdriRotation") ok = readFloats(rest.c_str(), &env.hdriRotation, 1);
            else if (key == "post") {
                const size_t sp = rest.find(' ');
                const std::string field = rest.substr(0, sp);
                bool known = false;
                for (const auto& [k, v] : postDefaults()) known |= k == field;
                if (known) env.setPost(field, sp == std::string::npos ? std::string() : rest.substr(sp + 1));
                else warn("unknown post value '" + field + "' dropped");
            } else warn("unknown key '" + key + "' dropped");
        }
        if (!ok) {
            error = "line " + std::to_string(lineNo) + ": invalid '" + key + "'";
            return false;
        }
    }
    if (inEntity) {
        error = "missing 'end' at end of file";
        return false;
    }
    u32 maxId = 0;
    for (const Entity& ent : scene.entities) maxId = std::max(maxId, ent.id);
    for (Entity& ent : scene.entities)
        if (ent.id == 0) ent.id = ++maxId;
    scene.nextId = maxId + 1;
    for (Entity& ent : scene.entities) { // missing parents or cycles go to the top level, like the engine
        if (!ent.parent) continue;
        bool bad = !scene.find(ent.parent);
        for (u32 p = ent.parent, depth = 0; !bad && p; ++depth) {
            const Entity* parent = scene.find(p);
            if (!parent || p == ent.id || depth > scene.entities.size()) bad = true;
            else p = parent->parent;
        }
        if (bad) {
            if (warnings) warnings->push_back("entity " + std::to_string(ent.id) + " has an invalid parent, moved to the top level");
            ent.parent = 0;
        }
    }
    out = std::move(scene);
    return true;
}

bool loadScene(const std::string& path, Scene& out, std::string& error, std::vector<std::string>* warnings) {
    std::ifstream in(pathOf(path), std::ios::binary);
    if (!in) {
        error = "cannot open file";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return readScene(text, out, error, warnings);
}

bool saveScene(const std::string& path, const Scene& scene, std::string& error) {
    const std::string text = writeScene(scene);
    // Written next to the file, then moved over it (no half-written scene if the disk fills up)
    const std::filesystem::path target = pathOf(path);
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream outFile(temp, std::ios::binary | std::ios::trunc);
        if (!outFile || !outFile.write(text.data(), static_cast<std::streamsize>(text.size())) || !outFile.flush()) {
            error = "cannot write file";
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        error = "cannot replace the file";
        return false;
    }
    return true;
}

const std::vector<KindInfo>& builtinKinds() {
    static const std::vector<KindInfo> kinds = {
        {kKindSolid, "Solid box", false, false},         {kKindDecoration, "Decoration (no collision)", false, false},
        {kKindSpawn, "Spawn point", true, false},        {kKindProbe, "Reflection probe", false, true},
        {kKindPointLight, "Point light", true, false},   {kKindReverb, "Reverb zone", false, true},
        {kKindCamera, "Virtual camera", true, false},    {kKindTerrain, "Terrain", false, false},
        {kKindScatter, "Scatter layer", false, false},   {kKindSprite, "Sprite", false, false},
    };
    return kinds;
}

const KindInfo* findKind(const std::string& name) {
    for (const KindInfo& k : builtinKinds())
        if (name == k.name) return &k;
    return nullptr;
}

TextFields defaultFields(const std::string& kind) {
    if (kind == kKindPointLight) return {{"color", "#FFD6A0"}, {"intensity", "3"}, {"range", "6"}};
    if (kind == kKindReverb) return {{"roomSize", "0.6"}, {"damping", "0.5"}, {"wet", "0.35"}, {"width", "1"}, {"fade", "0.8"}};
    if (kind == kKindTerrain) return {{"heightmap", ""}};
    if (kind == kKindSprite)
        return {{"image", ""},  {"columns", "1"}, {"rows", "1"},   {"cell", "0"},      {"pivotX", "0.5"},   {"pivotY", "0.5"},
                {"pixelsPerUnit", "0"}, {"layer", "0"}, {"flipX", "false"}, {"flipY", "false"}, {"pixelArt", "false"}};
    if (kind == kKindScatter)
        return {{"model", ""},   {"file", ""},         {"density", "0.5"},  {"minScale", "0.8"},
                {"maxScale", "1.2"}, {"randomYaw", "true"}, {"drawDistance", "120"}};
    if (kind == kKindCamera)
        return {{"body", "ThirdPerson"}, {"priority", "10"},     {"active", "true"},   {"follow", ""},         {"lookAt", ""},
                {"height", "0.8"},       {"distance", "18"},     {"minDistance", "4"}, {"maxDistance", "80"},  {"yaw", "45"},
                {"pitch", "30"},         {"minPitch", "-6"},     {"maxPitch", "83"},   {"orbitSpeed", "5"},    {"fov", "0"},
                {"deadZone", "0"},       {"damping", "0"},       {"collisionRadius", "0.3"}, {"blendSeconds", "0.8"},
                {"ease", "EaseInOut"},   {"rail", ""},           {"moveSpeed", "10"}};
    return {};
}

} // namespace mkrender::tool

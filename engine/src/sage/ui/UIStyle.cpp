#include "sage/ui/UIStyle.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "sage/assets/AssetDatabase.h"
#include "sage/core/Paths.h"
#include "sage/scene/Scene.h"
#include "sage/ui/Element.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UISerialize.h"

using json = nlohmann::json;

namespace sage::ui {

json CaptureStyle(const entt::registry& reg, entt::entity e) {
    json out;
    out["sage_ui_style"] = 1;
    if (const Element* el = reg.try_get<Element>(e)) out["type"] = el->Type;
    json& parts = out["parts"];
    parts = json::object();
    for (const PartType& p : Parts()) {
        if (!p.Fields || !p.Id || !p.Has || !p.Has(reg, e)) continue;
        json pj = json::object();
        SavePartFields(pj, p.Get(reg, e), *p.Fields, /*styleOnly=*/true);
        if (!pj.empty()) parts[p.Id] = pj;
    }
    return out;
}

void ApplyStyle(const json& style, entt::registry& reg, entt::entity e) {
    if (!style.is_object() || !style.contains("parts") || !style["parts"].is_object()) return;
    const json& parts = style["parts"];
    for (const PartType& p : Parts()) {
        if (!p.Fields || !p.Id || !parts.contains(p.Id) || !p.Has || !p.Has(reg, e)) continue;
        if (void* data = p.GetMutable(reg, e))
            LoadPartFields(parts[p.Id], data, *p.Fields, /*styleOnly=*/true);
    }
}

bool SaveStyleFile(const std::string& path, const json& style, std::string* err) {
    std::ofstream f(sage::PathFromUtf8(path), std::ios::binary);
    if (!f) {
        if (err) *err = "не открылся на запись: " + path;
        return false;
    }
    f << style.dump(2) << "\n";
    return (bool)f;
}

bool LoadStyleFile(const std::string& path, json& out, std::string* err) {
    std::ifstream f(sage::PathFromUtf8(path), std::ios::binary);
    if (!f) {
        if (err) *err = "не открылся: " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    out = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (out.is_discarded() || !out.is_object()) {
        if (err) *err = "не JSON стиля: " + path;
        return false;
    }
    return true;
}

namespace {

struct CachedStyle {
    json Data;
    int Version = 0;
    std::filesystem::file_time_type Stamp{};
    std::chrono::steady_clock::time_point Checked{};
    bool Loaded = false;
};

std::unordered_map<std::string, CachedStyle>& Cache() {
    static std::unordered_map<std::string, CachedStyle> cache;
    return cache;
}

// Прочитать, если файл новее прочитанного. Версия растёт с каждым чтением —
// по ней элементы узнают, что стиль пора положить заново.
CachedStyle& Refresh(const std::string& path) {
    CachedStyle& c = Cache()[path];
    const auto now = std::chrono::steady_clock::now();
    if (c.Loaded && now - c.Checked < std::chrono::milliseconds(500)) return c;
    c.Checked = now;
    const std::string real = sage::AssetDatabase::Instance().LocatePath(path);
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(sage::PathFromUtf8(real), ec);
    if (ec) return c;   // файла нет — остаётся то, что было (или ничего)
    if (c.Loaded && stamp == c.Stamp) return c;
    json data;
    if (LoadStyleFile(real, data)) {
        c.Data = std::move(data);
        c.Stamp = stamp;
        c.Loaded = true;
        ++c.Version;
    }
    return c;
}

} // namespace

void ApplyStyles(Scene& scene) {
    entt::registry& reg = scene.Registry();
    for (entt::entity e : reg.view<Element>()) {
        Element& el = reg.get<Element>(e);
        if (el.Style.empty()) continue;
        CachedStyle& c = Refresh(el.Style);
        if (!c.Loaded || el.StyleVersion == c.Version) continue;
        ApplyStyle(c.Data, reg, e);
        // Версия запоминается и у элемента, копия которого живёт в другом
        // реестре: у них свой StyleVersion, и каждый получит стиль сам.
        reg.get<Element>(e).StyleVersion = c.Version;
    }
}

void ClearStyleCache() { Cache().clear(); }

} // namespace sage::ui

#include "sage/ui/UIStyle.h"
#include "sage/assets/Pack.h"

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
    const Element* el = reg.try_get<Element>(e);
    for (const PartType& p : Parts()) {
        if (!p.Fields || !p.Id || !parts.contains(p.Id) || !p.Has || !p.Has(reg, e)) continue;
        // Переопределённые у элемента поля стиль не трогает.
        json pj = parts[p.Id];
        if (el && !el->StyleOverrides.empty() && pj.is_object()) {
            const std::string prefix = std::string(p.Id) + ".";
            for (const PartField& f : *p.Fields) {
                if (!f.Key) continue;
                const std::string key = prefix + f.Key;
                if (el->Overrides(key)) pj.erase(f.Key);
                else if (f.Type == PartField::Kind::Look) {
                    auto nested = pj.find(f.Key);
                    if (nested == pj.end() || !nested->is_object()) continue;
                    for (const auto& field : LookFields())
                        if (el->Overrides(key + "." + field.Key)) nested->erase(field.Key);
                }
            }
        }
        if (void* data = p.GetMutable(reg, e)) LoadPartFields(pj, data, *p.Fields, /*styleOnly=*/true);
    }
}

json MergeStyles(const json& base, const json& own) {
    json out = own;
    if (!base.is_object() || !base.contains("parts")) return out;
    json parts = base["parts"];
    if (own.contains("parts") && own["parts"].is_object())
        for (auto it = own["parts"].begin(); it != own["parts"].end(); ++it) {
            if (!parts.contains(it.key()) || !parts[it.key()].is_object()) parts[it.key()] = json::object();
            for (auto f = it.value().begin(); f != it.value().end(); ++f) parts[it.key()][f.key()] = f.value();
        }
    out["parts"] = parts;
    return out;
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
    std::string text;   // через vfs — стиль едет в пакете
    if (!sage::assets::vfs::ReadText(path, text)) {
        if (err) *err = "не открылся: " + path;
        return false;
    }
    out = json::parse(text, nullptr, /*allow_exceptions=*/false);
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

// Стиль с учётом НАСЛЕДОВАНИЯ: «base» — путь к родительскому стилю; его
// поля — основа, свои поля стиля поверх. Версия — общая на всю цепочку:
// правка родителя обновляет и элементы со стилем-наследником.
bool ResolveStyle(const std::string& path, json& out, int& version, int depth = 0) {
    CachedStyle& c = Refresh(path);
    if (!c.Loaded) return false;
    out = c.Data;
    version = c.Version;
    const std::string base = c.Data.value("base", std::string());
    // Цепочка не длиннее восьми: стиль, ссылающийся сам на себя, не вешает кадр.
    if (base.empty() || depth >= 8) return true;
    json parent;
    int parentVersion = 0;
    if (ResolveStyle(base, parent, parentVersion, depth + 1)) {
        out = MergeStyles(parent, c.Data);
        version = version * 7919 + parentVersion;
    }
    return true;
}

void ApplyStyles(Scene& scene) {
    entt::registry& reg = scene.Registry();
    for (entt::entity e : reg.view<Element>()) {
        Element& el = reg.get<Element>(e);
        if (el.Style.empty()) continue;
        json style;
        int version = 0;
        if (!ResolveStyle(el.Style, style, version) || el.StyleVersion == version) continue;
        ApplyStyle(style, reg, e);
        // Версия запоминается и у элемента, копия которого живёт в другом
        // реестре: у них свой StyleVersion, и каждый получит стиль сам.
        reg.get<Element>(e).StyleVersion = version;
    }
}

void ClearStyleCache() { Cache().clear(); }

} // namespace sage::ui

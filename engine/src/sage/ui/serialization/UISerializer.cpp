#include "sage/ui/serialization/UISerializer.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "sage/core/Log.h"
#include "sage/ui/core/UINode.h"
#include "sage/ui/core/UIRegistry.h"
#include "sage/ui/effects/UIEffect.h"
#include "sage/ui/layout/UITransform.h"
#include "sage/ui/serialization/UIPrefab.h"
#include "sage/ui/style/UIStyle.h"
#include "sage/ui/visual/UIFill.h"

using nlohmann::json;

// ---------------------------------------------------------------------------
// ФОРМАТ ДОКУМЕНТА.
//
// Ни одного «поле за полем» в этом файле нет: он ходит по таблицам свойств
// компонентов. Поэтому свойство, добавленное в компонент, сохраняется само, а
// забыть его негде (§64).
//
// Здесь же лежат SaveCustom/LoadCustom тех компонентов, которые хранят не
// плоские поля (стек эффектов, отличия префаба, чужие данные). Они определены
// именно тут, а не рядом со своими структурами, ровно по одной причине: json —
// внутренняя зависимость движка, и её не должно быть видно в публичных
// заголовках интерфейса.
// ---------------------------------------------------------------------------
namespace sage::ui {

namespace {

json SaveVec2(const glm::vec2& v) { return json::array({v.x, v.y}); }
json SaveVec4(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }

glm::vec2 LoadVec2(const json& j, const glm::vec2& def) {
    if (!j.is_array() || j.size() < 2) return def;
    return {j[0].get<float>(), j[1].get<float>()};
}
glm::vec4 LoadVec4(const json& j, const glm::vec4& def) {
    if (!j.is_array() || j.size() < 4) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
}

json SaveGradient(const UIGradient& g) {
    json j;
    j["Type"] = (int)g.Type;
    j["Angle"] = g.Angle;
    j["Center"] = SaveVec2(g.Center);
    j["Radius"] = g.Radius;
    json stops = json::array();
    for (const UIGradientStop& s : g.Stops)
        stops.push_back(json{{"P", s.Position}, {"C", SaveVec4(s.Color)}});
    j["Stops"] = stops;
    return j;
}

UIGradient LoadGradient(const json& j) {
    UIGradient g;
    if (!j.is_object()) return g;
    if (j.contains("Type")) g.Type = (UIGradient::Kind)j["Type"].get<int>();
    if (j.contains("Angle")) g.Angle = j["Angle"].get<float>();
    if (j.contains("Center")) g.Center = LoadVec2(j["Center"], g.Center);
    if (j.contains("Radius")) g.Radius = j["Radius"].get<float>();
    if (j.contains("Stops")) {
        for (const json& s : j["Stops"]) {
            UIGradientStop stop;
            stop.Position = s.value("P", 0.0f);
            stop.Color = LoadVec4(s.value("C", json()), stop.Color);
            g.Stops.push_back(stop);
        }
    }
    return g;
}

// Одно свойство → json. Работает ТОЛЬКО по таблице, поэтому никакой ветки на
// конкретный компонент здесь быть не может.
json SaveProperty(const void* data, const UIProperty& p) {
    switch (p.Type) {
        case UIProperty::Kind::Bool: return UIFieldAs<bool>(data, p);
        case UIProperty::Kind::Int:
        case UIProperty::Kind::Enum: return UIFieldAs<int>(data, p);
        case UIProperty::Kind::Float: return UIFieldAs<float>(data, p);
        case UIProperty::Kind::String: return UIFieldAs<std::string>(data, p);
        case UIProperty::Kind::Vec2: return SaveVec2(UIFieldAs<glm::vec2>(data, p));
        case UIProperty::Kind::Vec4:
        case UIProperty::Kind::Color: return SaveVec4(UIFieldAs<glm::vec4>(data, p));
        case UIProperty::Kind::Edges: {
            const UIEdges& e = UIFieldAs<UIEdges>(data, p);
            return json::array({e.L, e.T, e.R, e.B});
        }
        case UIProperty::Kind::Corners: {
            const UICorners& c = UIFieldAs<UICorners>(data, p);
            return json::array({c.TL, c.TR, c.BR, c.BL});
        }
        case UIProperty::Kind::Gradient: return SaveGradient(UIFieldAs<UIGradient>(data, p));
        default: return json();
    }
}

void LoadProperty(void* data, const UIProperty& p, const json& j) {
    // Значение неверного типа не портит компонент: остаётся то, что задал
    // конструктор (§134 — безопасный откат, а не «половина настроек мусор»).
    switch (p.Type) {
        case UIProperty::Kind::Bool:
            if (j.is_boolean()) UIFieldAs<bool>(data, p) = j.get<bool>();
            break;
        case UIProperty::Kind::Int:
        case UIProperty::Kind::Enum:
            if (j.is_number()) UIFieldAs<int>(data, p) = j.get<int>();
            break;
        case UIProperty::Kind::Float:
            if (j.is_number()) UIFieldAs<float>(data, p) = j.get<float>();
            break;
        case UIProperty::Kind::String:
            if (j.is_string()) UIFieldAs<std::string>(data, p) = j.get<std::string>();
            break;
        case UIProperty::Kind::Vec2:
            UIFieldAs<glm::vec2>(data, p) = LoadVec2(j, UIFieldAs<glm::vec2>(data, p));
            break;
        case UIProperty::Kind::Vec4:
        case UIProperty::Kind::Color:
            UIFieldAs<glm::vec4>(data, p) = LoadVec4(j, UIFieldAs<glm::vec4>(data, p));
            break;
        case UIProperty::Kind::Edges:
            if (j.is_array() && j.size() >= 4) {
                UIEdges& e = UIFieldAs<UIEdges>(data, p);
                e = UIEdges(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(),
                            j[3].get<float>());
            }
            break;
        case UIProperty::Kind::Corners:
            if (j.is_array() && j.size() >= 4) {
                UIFieldAs<UICorners>(data, p) = UICorners(j[0].get<float>(), j[1].get<float>(),
                                                          j[2].get<float>(), j[3].get<float>());
            }
            break;
        case UIProperty::Kind::Gradient:
            UIFieldAs<UIGradient>(data, p) = LoadGradient(j);
            break;
        default: break;
    }
}

json SaveComponent(const UIComponent& comp) {
    json j;
    j["type"] = comp.Type().Id;
    const void* data = comp.Data();
    for (const UIProperty& p : comp.Type().Props) {
        json value = SaveProperty(data, p);
        if (!value.is_null()) j[p.Key] = std::move(value);
    }
    comp.SaveCustom(&j);
    return j;
}

void LoadComponentFields(UIComponent& comp, const json& j) {
    void* data = comp.Data();
    for (const UIProperty& p : comp.Type().Props) {
        if (!j.contains(p.Key)) continue; // отсутствующее свойство = значение по умолчанию
        LoadProperty(data, p, j[p.Key]);
    }
    comp.LoadCustom(&j);
}

json SaveNode(const UIDocument& doc, const UINode& node) {
    json j;
    j["id"] = node.Id;
    j["guid"] = node.Guid;
    j["name"] = node.Name;
    if (!node.Enabled) j["enabled"] = false;
    if (!node.Visible) j["visible"] = false;
    if (node.Opacity != 1.0f) j["opacity"] = node.Opacity;
    if (node.Blend != UIBlendMode::Normal) j["blend"] = (int)node.Blend;
    if (node.Layer != 0) j["layer"] = node.Layer;
    if (node.Order != 0) j["order"] = node.Order;
    if (node.Locked) j["locked"] = true;
    if (node.EditorHidden) j["hidden"] = true;
    if (!node.PrefabSource.empty()) j["prefab"] = node.PrefabSource;

    json comps = json::array();
    for (const auto& c : node.Components) comps.push_back(SaveComponent(*c));
    j["components"] = comps;

    json kids = json::array();
    for (UINodeId cid : node.Children) {
        if (const UINode* child = doc.Find(cid)) kids.push_back(SaveNode(doc, *child));
    }
    if (!kids.empty()) j["children"] = kids;
    return j;
}

UINodeId LoadNode(UIDocument& doc, UINodeId parent, const json& j, UILoadReport& report) {
    UINode* node = doc.Create(j.value("name", std::string("Node")), parent);
    if (!node) return kUIInvalidNode;
    if (j.contains("guid")) node->Guid = j["guid"].get<std::string>();
    node->Enabled = j.value("enabled", true);
    node->Visible = j.value("visible", true);
    node->Opacity = j.value("opacity", 1.0f);
    node->Blend = (UIBlendMode)j.value("blend", 0);
    node->Layer = j.value("layer", 0);
    node->Order = j.value("order", 0);
    node->Locked = j.value("locked", false);
    node->EditorHidden = j.value("hidden", false);
    node->PrefabSource = j.value("prefab", std::string());
    ++report.Nodes;

    node->Components.clear();
    if (j.contains("components")) {
        for (const json& cj : j["components"]) {
            const std::string type = cj.value("type", std::string());
            if (type.empty()) continue;
            UIComponent* comp = node->Add(type);
            if (!comp) {
                // Чужой компонент НЕ ТЕРЯЕТСЯ (§65): его данные лежат как есть и
                // уезжают обратно в файл при следующем сохранении. Иначе один
                // открытый файл молча уничтожает чужую работу.
                UIUnknownComponent& unknown =
                    *static_cast<UIUnknownComponent*>(node->Add("unknown"));
                unknown.SourceId = type;
                unknown.Raw = cj.dump();
                report.Warnings.push_back("неизвестный компонент \"" + type + "\" сохранён как есть");
                continue;
            }
            LoadComponentFields(*comp, cj);
        }
    }
    // Прямоугольник обязателен: файл без него — сломанные данные, но не повод
    // терять узел целиком.
    if (!node->Get<UITransform>()) {
        node->Ensure<UITransform>();
        report.Warnings.push_back("узел \"" + node->Name + "\" без transform — добавлен пустой");
    }

    const UINodeId id = node->Id;
    if (j.contains("children")) {
        for (const json& cj : j["children"]) LoadNode(doc, id, cj, report);
    }
    return id;
}

json SaveCanvas(const UICanvasSettings& c) {
    json j;
    j["space"] = (int)c.Kind;
    j["scale"] = (int)c.Scale;
    j["aspect"] = (int)c.Aspect;
    j["reference"] = SaveVec2(c.Reference);
    j["match"] = c.MatchWidthOrHeight;
    j["userScale"] = c.UserScale;
    j["sortOrder"] = c.SortOrder;
    j["safeArea"] = json::array({c.SafeArea.L, c.SafeArea.T, c.SafeArea.R, c.SafeArea.B});
    j["respectSafeArea"] = c.RespectSafeArea;
    return j;
}

void LoadCanvas(UICanvasSettings& c, const json& j) {
    if (!j.is_object()) return;
    c.Kind = (UICanvasSettings::Space)j.value("space", 0);
    c.Scale = (UICanvasSettings::ScaleMode)j.value("scale", 1);
    c.Aspect = (UICanvasSettings::AspectMode)j.value("aspect", 0);
    c.Reference = LoadVec2(j.value("reference", json()), c.Reference);
    c.MatchWidthOrHeight = j.value("match", 0.5f);
    c.UserScale = j.value("userScale", 1.0f);
    c.SortOrder = j.value("sortOrder", 0);
    if (j.contains("safeArea") && j["safeArea"].is_array() && j["safeArea"].size() >= 4) {
        const json& s = j["safeArea"];
        c.SafeArea = UIEdges(s[0].get<float>(), s[1].get<float>(), s[2].get<float>(),
                             s[3].get<float>());
    }
    c.RespectSafeArea = j.value("respectSafeArea", false);
}

json SaveStyleValues(const std::vector<UIStyleValue>& values) {
    json arr = json::array();
    for (const UIStyleValue& v : values) {
        json j;
        j["c"] = v.Component;
        j["p"] = v.Property;
        if (v.IsText) j["t"] = v.Text;
        else j["n"] = SaveVec4(v.Numbers);
        arr.push_back(std::move(j));
    }
    return arr;
}

std::vector<UIStyleValue> LoadStyleValues(const json& arr) {
    std::vector<UIStyleValue> out;
    if (!arr.is_array()) return out;
    for (const json& j : arr) {
        UIStyleValue v;
        v.Component = j.value("c", std::string());
        v.Property = j.value("p", std::string());
        if (j.contains("t")) { v.Text = j["t"].get<std::string>(); v.IsText = true; }
        else v.Numbers = LoadVec4(j.value("n", json()), glm::vec4(0.0f));
        out.push_back(std::move(v));
    }
    return out;
}

// --- Переезд со старых версий (§65) -----------------------------------------
//
// Цепочка «из версии N в N+1». Пока версия одна, цепочка пуста — но её место
// уже есть, и добавление шага не требует трогать чтение.
void MigrateDocument(json& j, int fromVersion, UILoadReport& report) {
    int v = fromVersion;
    while (v < UIDocument::FormatVersion()) {
        switch (v) {
            // case 1: ...превратить формат 1 в формат 2...
            default: break;
        }
        ++v;
    }
    if (fromVersion != UIDocument::FormatVersion()) {
        report.Warnings.push_back("документ версии " + std::to_string(fromVersion) +
                                  " прочитан как версия " +
                                  std::to_string(UIDocument::FormatVersion()));
    }
    (void)j;
}

} // namespace

// --- Компоненты со своими данными -------------------------------------------

bool UIEffects::SaveCustom(void* jsonObject) const {
    json& j = *static_cast<json*>(jsonObject);
    json arr = json::array();
    for (const auto& e : Items) {
        json ej;
        ej["type"] = e->Type().Id;
        ej["enabled"] = e->Enabled;
        const void* data = e->Data();
        for (const UIProperty& p : e->Type().Props) {
            json value = SaveProperty(data, p);
            if (!value.is_null()) ej[p.Key] = std::move(value);
        }
        arr.push_back(std::move(ej));
    }
    j["items"] = std::move(arr);
    return true;
}

bool UIEffects::LoadCustom(const void* jsonObject) {
    const json& j = *static_cast<const json*>(jsonObject);
    Items.clear();
    if (!j.contains("items")) return false;
    for (const json& ej : j["items"]) {
        const std::string type = ej.value("type", std::string());
        UIEffect* e = Add(type);
        if (!e) continue; // неизвестный эффект пропускается, но документ цел
        e->Enabled = ej.value("enabled", true);
        void* data = e->Data();
        for (const UIProperty& p : e->Type().Props) {
            if (ej.contains(p.Key)) LoadProperty(data, p, ej[p.Key]);
        }
    }
    return true;
}

bool UIStyled::SaveCustom(void* jsonObject) const {
    json& j = *static_cast<json*>(jsonObject);
    if (Overrides.empty()) return false;
    j["overrides"] = Overrides;
    return true;
}

bool UIStyled::LoadCustom(const void* jsonObject) {
    const json& j = *static_cast<const json*>(jsonObject);
    Overrides.clear();
    if (!j.contains("overrides")) return false;
    for (const json& o : j["overrides"]) Overrides.push_back(o.get<std::string>());
    return true;
}

bool UIPrefabInstance::SaveCustom(void* jsonObject) const {
    json& j = *static_cast<json*>(jsonObject);
    json arr = json::array();
    for (const UIPrefabOverride& o : Overrides)
        arr.push_back(json{{"path", o.Path}, {"value", o.Value}});
    j["overrides"] = std::move(arr);
    return true;
}

bool UIPrefabInstance::LoadCustom(const void* jsonObject) {
    const json& j = *static_cast<const json*>(jsonObject);
    Overrides.clear();
    if (!j.contains("overrides")) return false;
    for (const json& o : j["overrides"]) {
        UIPrefabOverride ov;
        ov.Path = o.value("path", std::string());
        ov.Value = o.value("value", std::string());
        Overrides.push_back(std::move(ov));
    }
    return true;
}

const UIComponentType& UIUnknownComponent::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "unknown";
        d.Title = SAGE_UI_TEXT("Unknown component");
        d.Hint = SAGE_UI_TEXT("Data from a component this build does not have — kept as is");
        d.Icon = "warn";
        d.Category = UIComponentCategory::Advanced;
        d.Order = 999;
        d.Unique = false;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIUnknownComponent()); };
        return d;
    }();
    return t;
}

bool UIUnknownComponent::SaveCustom(void* jsonObject) const {
    json& j = *static_cast<json*>(jsonObject);
    // Записываем обратно ИСХОДНЫЙ объект целиком, включая его настоящий тип:
    // сборка, у которой этот компонент есть, обязана прочитать файл без потерь.
    if (Raw.empty()) return false;
    json parsed = json::parse(Raw, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return false;
    j = std::move(parsed);
    return true;
}

bool UIUnknownComponent::LoadCustom(const void* jsonObject) {
    const json& j = *static_cast<const json*>(jsonObject);
    Raw = j.dump();
    SourceId = j.value("type", std::string());
    return true;
}

// --- Документ ---------------------------------------------------------------

std::string UISaveDocumentToString(const UIDocument& doc, const UITheme* theme) {
    json j;
    j["ui_version"] = UIDocument::FormatVersion();
    j["name"] = doc.Name();
    j["canvas"] = SaveCanvas(doc.Canvas());

    json roots = json::array();
    for (UINodeId id : doc.Roots()) {
        if (const UINode* n = doc.Find(id)) roots.push_back(SaveNode(doc, *n));
    }
    j["nodes"] = std::move(roots);
    if (theme) j["theme"] = json::parse(UISaveThemeToString(*theme));
    return j.dump(2);
}

UILoadReport UILoadDocumentFromString(UIDocument& doc, const std::string& text, UITheme* theme) {
    UILoadReport report;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        report.Error = "документ не разбирается как JSON";
        return report;
    }
    report.FromVersion = j.value("ui_version", 1);
    if (report.FromVersion > UIDocument::FormatVersion()) {
        // Файл из БОЛЕЕ НОВОЙ сборки. Читаем как умеем и честно предупреждаем:
        // отказ читать вовсе хуже — человек теряет доступ к своей работе.
        report.Warnings.push_back("документ новее этой сборки (версия " +
                                  std::to_string(report.FromVersion) +
                                  ") — часть свойств может быть не прочитана");
    } else {
        MigrateDocument(j, report.FromVersion, report);
    }

    doc.Clear();
    doc.SetName(j.value("name", std::string()));
    LoadCanvas(doc.Canvas(), j.value("canvas", json()));
    if (j.contains("nodes")) {
        for (const json& nj : j["nodes"]) LoadNode(doc, kUIInvalidNode, nj, report);
    }
    if (theme && j.contains("theme")) UILoadThemeFromString(*theme, j["theme"].dump());
    doc.MarkDirty(UIDirty_All);
    report.Ok = true;
    return report;
}

bool UISaveDocument(const UIDocument& doc, const std::string& path, const UITheme* theme) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        LOG_ERROR("UI") << "не удалось открыть для записи: " << path;
        return false;
    }
    out << UISaveDocumentToString(doc, theme);
    return true;
}

UILoadReport UILoadDocument(UIDocument& doc, const std::string& path, UITheme* theme) {
    UILoadReport report;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        report.Error = "файл не открывается: " + path;
        return report;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    report = UILoadDocumentFromString(doc, ss.str(), theme);
    for (const std::string& w : report.Warnings) LOG_WARN("UI") << path << ": " << w;
    if (!report.Error.empty()) LOG_ERROR("UI") << path << ": " << report.Error;
    return report;
}

std::string UISaveSubtree(const UIDocument& doc, UINodeId root) {
    const UINode* n = doc.Find(root);
    if (!n) return "{}";
    json j;
    j["ui_version"] = UIDocument::FormatVersion();
    j["nodes"] = json::array({SaveNode(doc, *n)});
    return j.dump(2);
}

UINodeId UILoadSubtree(UIDocument& doc, UINodeId parent, const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.contains("nodes")) return kUIInvalidNode;
    UILoadReport report;
    UINodeId first = kUIInvalidNode;
    for (const json& nj : j["nodes"]) {
        const UINodeId id = LoadNode(doc, parent, nj, report);
        if (first == kUIInvalidNode) first = id;
    }
    // Guid'ы вставленной копии обязаны стать своими: иначе две «одинаковые»
    // кнопки в одном документе, и ссылки указывают неизвестно на какую.
    if (first != kUIInvalidNode) {
        doc.Traverse(first, [&](UINode& n) {
            n.Guid = doc.MakeGuid();
            return true;
        });
    }
    return first;
}

// --- Тема -------------------------------------------------------------------

std::string UISaveThemeToString(const UITheme& theme) {
    json j;
    j["name"] = theme.Name;
    json colors;
    for (const auto& [k, v] : theme.Tokens.Colors()) colors[k] = UIColorToHex(v);
    json numbers;
    for (const auto& [k, v] : theme.Tokens.Numbers()) numbers[k] = v;
    json strings;
    for (const auto& [k, v] : theme.Tokens.Strings()) strings[k] = v;
    j["tokens"] = json{{"colors", colors}, {"numbers", numbers}, {"strings", strings}};

    json styles;
    for (const auto& [name, style] : theme.Styles) {
        json sj;
        if (!style.Parent.empty()) sj["parent"] = style.Parent;
        sj["values"] = SaveStyleValues(style.Values);
        json states;
        for (const auto& [state, values] : style.States) states[state] = SaveStyleValues(values);
        if (!states.is_null()) sj["states"] = states;
        styles[name] = std::move(sj);
    }
    j["styles"] = std::move(styles);
    return j.dump(2);
}

bool UILoadThemeFromString(UITheme& theme, const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    theme.Name = j.value("name", std::string());
    theme.Tokens.Clear();
    theme.Styles.clear();

    if (j.contains("tokens")) {
        const json& t = j["tokens"];
        if (t.contains("colors"))
            for (auto it = t["colors"].begin(); it != t["colors"].end(); ++it)
                theme.Tokens.SetColor(it.key(), UIColorFromHex(it.value().get<std::string>()));
        if (t.contains("numbers"))
            for (auto it = t["numbers"].begin(); it != t["numbers"].end(); ++it)
                theme.Tokens.SetNumber(it.key(), it.value().get<float>());
        if (t.contains("strings"))
            for (auto it = t["strings"].begin(); it != t["strings"].end(); ++it)
                theme.Tokens.SetString(it.key(), it.value().get<std::string>());
    }
    if (j.contains("styles")) {
        for (auto it = j["styles"].begin(); it != j["styles"].end(); ++it) {
            UIStyle& s = theme.Ensure(it.key());
            s.Parent = it.value().value("parent", std::string());
            s.Values = LoadStyleValues(it.value().value("values", json()));
            if (it.value().contains("states")) {
                const json& states = it.value()["states"];
                for (auto sit = states.begin(); sit != states.end(); ++sit)
                    s.States[sit.key()] = LoadStyleValues(sit.value());
            }
        }
    }
    return true;
}

} // namespace sage::ui

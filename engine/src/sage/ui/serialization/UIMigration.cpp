#include "sage/ui/serialization/UIMigration.h"

#include <fstream>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "sage/core/Log.h"
#include "sage/ui/UIFramework.h"
#include "sage/ui/visual/UIIcon.h"

using nlohmann::json;

namespace sage::ui {

namespace {

glm::vec2 Vec2Of(const json& j, glm::vec2 def) {
    if (!j.is_object()) return def;
    return {j.value("x", def.x), j.value("y", def.y)};
}
glm::vec4 Vec4Of(const json& j, glm::vec4 def) {
    if (!j.is_object()) return def;
    return {j.value("x", def.x), j.value("y", def.y), j.value("z", def.z), j.value("w", def.w)};
}

// Старый якорь (0..8) → доли. Девять точек — частный случай долей, поэтому
// перевод однозначен и не теряет ничего.
glm::vec2 AnchorFraction(int anchor) {
    const int col = anchor % 3, row = anchor / 3;
    return {col == 0 ? 0.0f : col == 1 ? 0.5f : 1.0f,
            row == 0 ? 0.0f : row == 1 ? 0.5f : 1.0f};
}

struct Context {
    UIDocument* Doc = nullptr;
    UIMigrationReport* Report = nullptr;
};

// Общая часть обоих старых форматов: прямоугольник.
void ApplyTransform(UINode& node, UITransform& t, const json& src, bool flat) {
    const int anchor = src.value("anchor", 0);
    const glm::vec2 a = AnchorFraction(anchor >= 0 && anchor <= 8 ? anchor : 0);
    t.AnchorMin = t.AnchorMax = a;
    t.Offset = Vec2Of(src.value("offset", json()), t.Offset);
    t.Size = Vec2Of(src.value("size", json()), t.Size);
    // Опорная точка = точка якоря плюс старый «сдвиг на долю размера»: старая
    // формула складывала их ровно так, и без этого элементы уехали бы.
    t.Pivot = a + Vec2Of(src.value("pivot", json()), glm::vec2(0.0f));
    if (src.contains("margin")) {
        const glm::vec4 m = Vec4Of(src["margin"], glm::vec4(0.0f));
        t.Margin = UIEdges(m.x, m.y, m.z, m.w);
    }
    const int stretch = src.value("stretch", 0);
    if (stretch == 1) t.SetStretch(true, false);
    else if (stretch == 2) t.SetStretch(false, true);
    else if (stretch == 3) t.SetStretch(true, true);

    node.Order = src.value("layer", 0);
    node.Visible = src.value("visible", true);
    (void)flat;
}

void AddFill(UINode& node, const json& src, const char* colorKey, Context& ctx) {
    UIFill& fill = node.Ensure<UIFill>();
    fill.Color = Vec4Of(src.value(colorKey, json()), fill.Color);
    fill.Radius = UICorners(src.value("rounding", 0.0f));
    ++ctx.Report->Components;

    const float border = src.value("borderThickness", 0.0f);
    if (border > 0.0f) {
        // Рамка была двумя полями внутри заливки — теперь свой компонент, и у
        // неё появляются собственные скругления и стороны.
        UIBorder& b = node.Ensure<UIBorder>();
        b.Thickness = UIEdges::Uniform(border);
        b.Color = Vec4Of(src.value("borderColor", json()), b.Color);
    }
    const glm::vec4 gradient = Vec4Of(src.value("gradient", src.value("gradientColor", json())),
                                      glm::vec4(0.0f));
    if (gradient.a > 0.0f) {
        fill.Type = UIFill::Kind::Gradient;
        fill.Gradient = UIGradient::TwoColor(fill.Color, gradient, 0.0f);
    }
    const float shadow = src.value("shadowSize", 0.0f);
    if (shadow > 0.0f) {
        // Тень была парой полей — теперь эффект в стеке, и рядом с ней можно
        // поставить вторую, свечение или размытие.
        UIDropShadow& s = node.Ensure<UIEffects>().Ensure<UIDropShadow>();
        s.Blur = shadow;
        s.Offset = {0.0f, shadow * 0.35f};
        s.Color = Vec4Of(src.value("shadowColor", json()), s.Color);
    }
}

void AddImage(UINode& node, const json& src, const char* pathKey, Context& ctx) {
    const std::string path = src.value(pathKey, std::string());
    if (path.empty()) return;
    UIImage& img = node.Ensure<UIImage>();
    img.Path = path;
    img.Tint = Vec4Of(src.value("tint", json()), img.Tint);
    img.SourceRect = Vec4Of(src.value("sprite", json()), glm::vec4(0.0f));
    const glm::vec4 slice = Vec4Of(src.value("sliceBorder", json()), glm::vec4(0.0f));
    img.Slice = UIEdges(slice.x, slice.y, slice.z, slice.w);
    img.SliceScale = src.value("pixelScale", 0.0f);
    img.PixelArt = src.value("pixelArt", false);
    ++ctx.Report->Components;

    const glm::vec4 hover = Vec4Of(src.value("spriteHover", json()), glm::vec4(0.0f));
    const glm::vec4 pressed = Vec4Of(src.value("spritePressed", json()), glm::vec4(0.0f));
    if (hover.z > 0.0f || pressed.z > 0.0f) {
        // Спрайты состояний в новой системе — дело СТИЛЯ состояния: так их
        // можно задать любому свойству, а не только спрайту.
        ctx.Report->Warnings.push_back(
            node.Name + ": спрайты состояний не перенесены — задайте их стилем состояния");
    }
}

void AddText(UINode& node, const json& src, Context& ctx) {
    UIText& text = node.Ensure<UIText>();
    text.Text = src.value("text", std::string());
    // Старый «масштаб» был множителем к номинальным восьми пикселям.
    text.Size = src.value("scale", src.value("textScale", 2.0f)) * 8.0f;
    text.Color = Vec4Of(src.value("color", src.value("textColor", json())), text.Color);
    auto align = [](int v) {
        return v == 0 ? UITextAlign::Left : v == 2 ? UITextAlign::Right : UITextAlign::Center;
    };
    auto valign = [](int v) {
        return v == 0 ? UITextVAlign::Top : v == 2 ? UITextVAlign::Bottom : UITextVAlign::Center;
    };
    text.Align = align(src.value("horizontal", 1));
    text.VAlign = valign(src.value("vertical", 1));
    if (src.contains("textCentered"))
        text.Align = src["textCentered"].get<bool>() ? UITextAlign::Center : UITextAlign::Left;
    text.Wrap = src.value("wrap", src.value("wrapText", false)) ? UITextWrap::Word
                                                                : UITextWrap::None;
    const float pad = src.value("padX", 8.0f);
    text.Padding = UIEdges(pad, 0.0f, pad, 0.0f);
    ++ctx.Report->Components;
    if (src.value("autoWidth", false)) {
        if (UITransform* t = node.Get<UITransform>()) t->WidthMode = UISizeMode::Content;
        ctx.Report->Warnings.push_back(node.Name + ": авто-ширина стала размером по содержимому");
    }
}

void AddInteraction(UINode& node, const json& src, Context& ctx) {
    UIInteraction& ia = node.Ensure<UIInteraction>();
    ia.Enabled = src.value("enabled", true);
    ia.Cursor = src.value("cursor", std::string());
    ia.Command = src.value("action", std::string());
    ia.Focusable = true;
    ia.Hit = UIHitShape::RoundedRect;
    ++ctx.Report->Components;
    if (src.contains("events") && src["events"].is_array() && !src["events"].empty()) {
        // Связи «событие → метод объекта» — часть игровой сцены, а не документа
        // интерфейса: новый UI сообщает КОМАНДУ, а кто её слушает, решает игра.
        ctx.Report->Warnings.push_back(
            node.Name + ": связи событий не перенесены — подпишитесь на команду \"" +
            ia.Command + "\" в игре");
    }
}

// --- Плоский формат ("kind") -------------------------------------------------
void ConvertFlat(UINode& node, const json& uj, Context& ctx) {
    UITransform& t = node.Ensure<UITransform>();
    ApplyTransform(node, t, uj, /*flat=*/true);

    const std::string kind = uj.value("kind", "panel");
    if (uj.value("clipChildren", false)) {
        UIMask& mask = node.Ensure<UIMask>();
        mask.Form = uj.value("rounding", 0.0f) > 0.0f ? UIMask::Shape::RoundedRect
                                                      : UIMask::Shape::Rect;
        mask.Radius = UICorners(uj.value("rounding", 0.0f));
        ++ctx.Report->Components;
    }

    if (kind == "label") {
        AddText(node, uj, ctx);
    } else if (kind == "image") {
        AddImage(node, uj, "texture", ctx);
    } else if (kind == "icon") {
        UIIcon& icon = node.Ensure<UIIcon>();
        icon.Name = uj.value("icon", std::string());
        icon.Color = Vec4Of(uj.value("iconColor", json()), icon.Color);
        ++ctx.Report->Components;
    } else if (kind == "bar") {
        AddFill(node, uj, "color", ctx);
        UIProgress& p = node.Ensure<UIProgress>();
        p.Value = uj.value("value", 1.0f);
        p.FillColor = Vec4Of(uj.value("barFillColor", json()), p.FillColor);
        p.TrackColor = UIColor(0.0f);
        ++ctx.Report->Components;
    } else if (kind == "slider" || kind == "checkbox") {
        AddFill(node, uj, "color", ctx);
        UIRangeValue& r = node.Ensure<UIRangeValue>();
        r.Min = uj.value("minValue", 0.0f);
        r.Max = uj.value("maxValue", 1.0f);
        r.Value = uj.value("value", 0.5f);
        r.Toggle = kind == "checkbox";
        if (r.Toggle) r.Step = 1.0f;
        ++ctx.Report->Components;
        AddInteraction(node, uj, ctx);
    } else if (kind == "input") {
        AddFill(node, uj, "color", ctx);
        UITextField& f = node.Ensure<UITextField>();
        f.Value = uj.value("text", std::string());
        f.PlaceholderKey = uj.value("placeholder", std::string());
        f.MaxLength = uj.value("maxLength", 0);
        f.Password = uj.value("password", false);
        AddText(node, uj, ctx);
        AddInteraction(node, uj, ctx);
        ++ctx.Report->Components;
    } else {
        AddFill(node, uj, "color", ctx);
        // У панели старой системы текст жил ВНУТРИ неё же. Теперь надпись —
        // отдельный узел: её видно в дереве, её можно подвинуть, покрасить или
        // убрать совсем.
        if (!uj.value("text", std::string()).empty()) {
            UINode& label = *ctx.Doc->Create("Text", node.Id);
            label.Ensure<UITransform>().SetStretch(true, true);
            AddText(label, uj, ctx);
            ++ctx.Report->Nodes;
        }
        if (!uj.value("texture", std::string()).empty()) AddImage(node, uj, "texture", ctx);
        if (uj.value("interactive", false)) AddInteraction(node, uj, ctx);
    }
}

// --- Покомпонентный формат ---------------------------------------------------
void ConvertParts(UINode& node, const json& uj, Context& ctx) {
    UITransform& t = node.Ensure<UITransform>();
    if (uj.contains("transform")) ApplyTransform(node, t, uj["transform"], /*flat=*/false);

    if (uj.contains("fill")) AddFill(node, uj["fill"], "color", ctx);
    if (uj.contains("image")) AddImage(node, uj["image"], "path", ctx);
    // Часть надписи называлась "label" — не "text": имя части и имя её поля
    // разные, и перепутать их значит молча потерять КАЖДУЮ надпись в сцене.
    if (uj.contains("label")) AddText(node, uj["label"], ctx);
    if (uj.contains("icon")) {
        const json& ij = uj["icon"];
        UIIcon& icon = node.Ensure<UIIcon>();
        icon.Name = ij.value("name", std::string());
        icon.Color = Vec4Of(ij.value("color", json()), icon.Color);
        icon.Size = ij.value("size", 0.0f);
        ++ctx.Report->Components;
    }
    if (uj.contains("bar")) {
        const json& bj = uj["bar"];
        UIProgress& p = node.Ensure<UIProgress>();
        p.Value = bj.value("value", 1.0f);
        p.FillColor = Vec4Of(bj.value("fillColor", json()), p.FillColor);
        p.TrackColor = UIColor(0.0f);
        const int grow = bj.value("grow", 0);
        p.Grow = grow == 1   ? UIProgress::Direction::RightToLeft
                 : grow == 2 ? UIProgress::Direction::BottomToTop
                 : grow == 3 ? UIProgress::Direction::TopToBottom
                             : UIProgress::Direction::LeftToRight;
        p.Smoothing = bj.value("smoothing", 0.0f);
        ++ctx.Report->Components;
    }
    if (uj.contains("mask")) {
        const json& mj = uj["mask"];
        UIMask& mask = node.Ensure<UIMask>();
        mask.Form = mj.value("form", 0) == 1 ? UIMask::Shape::RoundedRect : UIMask::Shape::Rect;
        float rounding = mj.value("rounding", -1.0f);
        if (rounding < 0.0f && uj.contains("fill")) rounding = uj["fill"].value("rounding", 0.0f);
        mask.Radius = UICorners(rounding > 0.0f ? rounding : 0.0f);
        const glm::vec4 pad = Vec4Of(mj.value("padding", json()), glm::vec4(0.0f));
        mask.Padding = UIEdges(pad.x, pad.y, pad.z, pad.w);
        ++ctx.Report->Components;
    }
    if (uj.contains("layout")) {
        const json& lj = uj["layout"];
        UILayout& layout = node.Ensure<UILayout>();
        const int dir = lj.value("direction", 1);
        layout.Kind = dir == 0   ? UILayout::Mode::Horizontal
                      : dir == 2 ? UILayout::Mode::Grid
                                 : UILayout::Mode::Vertical;
        const int justify = lj.value("justify", 0);
        layout.Main = justify == 1   ? UIAlign::Center
                      : justify == 2 ? UIAlign::End
                      : justify == 3 ? UIAlign::SpaceBetween
                                     : UIAlign::Start;
        layout.Cross = lj.value("stretchCross", true) ? UIAlign::Stretch : UIAlign::Start;
        const float spacing = lj.value("spacing", 8.0f);
        layout.Gap = {spacing, spacing};
        const glm::vec4 pad = Vec4Of(lj.value("padding", json()), glm::vec4(8.0f));
        layout.Padding = UIEdges(pad.x, pad.y, pad.z, pad.w);
        layout.Columns = lj.value("columns", 3);
        layout.FitWidth = layout.FitHeight = lj.value("fitContent", false);
        if (layout.FitWidth) {
            t.WidthMode = UISizeMode::Content;
            t.HeightMode = UISizeMode::Content;
        }
        ++ctx.Report->Components;
    }
    if (uj.contains("group")) {
        node.Opacity = uj["group"].value("alpha", 1.0f);
        ++ctx.Report->Components;
    }
    if (uj.contains("interactable")) AddInteraction(node, uj["interactable"], ctx);
    if (uj.contains("range")) {
        const json& rj = uj["range"];
        UIRangeValue& r = node.Ensure<UIRangeValue>();
        r.Min = rj.value("min", 0.0f);
        r.Max = rj.value("max", 1.0f);
        r.Value = rj.value("value", 0.5f);
        r.Step = rj.value("step", 0.0f);
        r.Toggle = rj.value("toggle", false);
        r.TrackColor = Vec4Of(rj.value("trackColor", json()), r.TrackColor);
        r.AccentColor = Vec4Of(rj.value("accentColor", json()), r.AccentColor);
        r.Radius = UICorners(rj.value("rounding", 4.0f));
        ++ctx.Report->Components;
    }
    if (uj.contains("textInput")) {
        const json& ij = uj["textInput"];
        UITextField& f = node.Ensure<UITextField>();
        f.PlaceholderKey = ij.value("placeholder", std::string());
        f.MaxLength = ij.value("maxLength", 0);
        f.Password = ij.value("password", false);
        f.ReadOnly = ij.value("readOnly", false);
        if (uj.contains("label")) f.Value = uj["label"].value("text", std::string());
        ++ctx.Report->Components;
    }
    if (uj.contains("canvas")) {
        const json& cj = uj["canvas"];
        UICanvasSettings& canvas = ctx.Doc->Canvas();
        canvas.Scale = cj.value("mode", 0) == 1 ? UICanvasSettings::ScaleMode::ScaleWithSize
                                                : UICanvasSettings::ScaleMode::Pixels;
        canvas.Reference = Vec2Of(cj.value("reference", json()), canvas.Reference);
        canvas.MatchWidthOrHeight = cj.value("matchWidthOrHeight", 0.5f);
        canvas.SortOrder = cj.value("sortOrder", 0);
    }
}

} // namespace

UIMigrationReport UIMigrateSceneText(const std::string& sceneJson, UIDocument& doc) {
    UIInitialize();
    UIMigrationReport report;
    doc.Clear();

    json scene = json::parse(sceneJson, nullptr, false);
    if (scene.is_discarded() || !scene.is_object()) {
        report.Error = "сцена не разбирается как JSON";
        return report;
    }
    if (!scene.contains("objects") || !scene["objects"].is_array()) {
        report.Error = "в сцене нет объектов";
        return report;
    }

    Context ctx{&doc, &report};

    // Иерархия старой сцены — по числовым id. Собираем два прохода: сначала
    // узлы, потом связи. Один проход не годится: родитель может быть записан
    // после ребёнка.
    std::unordered_map<int, UINodeId> byId;
    std::unordered_map<int, int> parentOf;

    for (const json& obj : scene["objects"]) {
        if (!obj.contains("ui")) continue;
        const int id = obj.value("id", -1);
        UINode& node = *doc.Create(obj.value("name", std::string("Element")));
        ++report.Nodes;
        const json& uj = obj["ui"];
        // Два разных формата, и делать вид, что один плавно переходит в другой,
        // значит получить третий.
        if (uj.contains("kind")) ConvertFlat(node, uj, ctx);
        else ConvertParts(node, uj, ctx);
        if (id >= 0) byId[id] = node.Id;
        const int parent = obj.value("parent", -1);
        if (parent >= 0 && id >= 0) parentOf[id] = parent;
    }

    for (const auto& [childId, parentId] : parentOf) {
        auto child = byId.find(childId);
        auto parent = byId.find(parentId);
        // Родитель без интерфейса (объект сцены) — ребёнок остаётся корнем:
        // в документе интерфейса нечему быть родителем надписи, кроме узла.
        if (child == byId.end() || parent == byId.end()) continue;
        doc.Reparent(child->second, parent->second);
    }

    doc.MarkDirty(UIDirty_All);
    report.Ok = true;
    return report;
}

UIMigrationReport UIMigrateSceneFile(const std::string& scenePath, UIDocument& doc) {
    std::ifstream in(scenePath, std::ios::binary);
    if (!in) {
        UIMigrationReport report;
        report.Error = "файл сцены не открывается: " + scenePath;
        LOG_ERROR("UI") << report.Error;
        return report;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    UIMigrationReport report = UIMigrateSceneText(ss.str(), doc);
    for (const std::string& w : report.Warnings) LOG_WARN("UI") << scenePath << ": " << w;
    return report;
}

} // namespace sage::ui

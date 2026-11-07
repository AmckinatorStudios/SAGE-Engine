#include "sage/scripting/ScriptEngine.h"

#include <filesystem>
#include <map>
#include <system_error>

#include "sage/assets/AssetDatabase.h"
#include "sage/scene/SceneComponents.h"
#include "sage/ui/SpriteSheet.h"
#include "sage/core/Paths.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"

// ---------------------------------------------------------------------------
// ИНТЕРФЕЙС В СТИЛЕ SAGE — ОДИН ПУТЬ ВМЕСТО ЧЕТЫРЁХ (см. docs/ui.md).
//
// До этого слоя у скрипта было четыре параллельных способа сделать одно и то
// же: прокси UIElement с полями в PascalCase (`obj:AddUI().Type = UIKind.Image`),
// глобальные функции SetUI*/sage.ui.*, таблица UI.find/get и свойства объекта
// (text, visible). Ни один из них не умел всего: вид кнопки при наведении из
// скрипта не задавался вовсе (поля Look не были выведены), поэтому игры
// каждый кадр сами меняли спрайт кнопки и перечитывали лист спрайтов с диска.
//
// Здесь — один набор строителей, по одному на вид элемента:
//
//      local menu = ui.screen{order = 10}
//      local play = ui.button{parent = menu, x = 0, y = 40, w = 200, h = 40,
//                             anchor = "top", text = "Play", on_click = start}
//      play.text = "Continue"
//      play.enabled = false
//
// Элемент — таблица со свойствами (присваиванием) и методами (через точку).
// Неизвестное свойство — ошибка с именем, а не молчаливая запись в пустоту.
// Прежние имена остаются: на них написаны игры (снимок API это сторожит).
//
// Под слоем Lua — две функции C++: вид части элемента (SetLook) и подписка на
// сигнал элемента (Connect). Остальное — имена и форма вызова.
// ---------------------------------------------------------------------------

namespace {

// Описание листа спрайтов — с ПАМЯТЬЮ. Игра меняет кусок листа на наведении
// и нажатии, и каждый такой вызов прежде читал и разбирал .sagesheet с диска
// заново. Перечитывается файл, только если он изменился (время записи);
// у файла из пакета времени нет — он и не меняется.
struct CachedSheet {
    sage::ui::SpriteSheet Sheet;
    std::filesystem::file_time_type Stamp{};
    bool Loaded = false;
};

const sage::ui::SpriteSheet* SheetFor(const std::string& imagePath) {
    static std::map<std::string, CachedSheet> cache;
    const std::string file =
        sage::ui::SpriteSheet::SidecarPath(sage::AssetDatabase::Instance().LocatePath(imagePath));
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(sage::PathFromUtf8(file), ec);
    CachedSheet& c = cache[file];
    if (!c.Loaded || (!ec && stamp != c.Stamp)) {
        std::string err;
        c.Sheet = sage::ui::SpriteSheet{};
        c.Loaded = sage::ui::SpriteSheet::LoadFile(file, c.Sheet, err);
        c.Stamp = ec ? std::filesystem::file_time_type{} : stamp;
        if (!c.Loaded) return nullptr;
    }
    return &c.Sheet;
}

glm::vec4 ColorArg(const sol::object& v, const char* what) {
    if (v.is<glm::vec4>()) return v.as<glm::vec4>();
    if (v.is<glm::vec3>()) return glm::vec4(v.as<glm::vec3>(), 1.0f);
    if (v.is<sol::table>()) {
        sol::table t = v.as<sol::table>();
        return {t.get_or(1, 1.0f), t.get_or(2, 1.0f), t.get_or(3, 1.0f), t.get_or(4, 1.0f)};
    }
    throw std::runtime_error(std::string("ui: ") + what + " — ожидается цвет (Vec4 или {r, g, b, a})");
}

glm::vec4 Vec4Arg(const sol::object& v, const char* what) {
    if (v.is<glm::vec4>()) return v.as<glm::vec4>();
    if (v.get_type() == sol::type::number) {
        const float n = v.as<float>();
        return {n, n, n, n};
    }
    if (v.is<sol::table>()) {
        sol::table t = v.as<sol::table>();
        return {t.get_or(1, 0.0f), t.get_or(2, 0.0f), t.get_or(3, 0.0f), t.get_or(4, 0.0f)};
    }
    throw std::runtime_error(std::string("ui: ") + what + " — ожидается {л, в, п, н} или число");
}

const char* const kFits[] = {"stretch", "nineslice", "tile", "fit", "cover"};

// Вид из таблицы. Ключи — те же, что у строителей элементов:
//   color, image, sprite (имя куска листа или {x, y, w, h}), slice, pixel,
//   fit, sharp, rounding, border, border_color, gradient, shadow.
// Отсутствующий ключ оставляет поле как было: так `hover = {sprite = "b2"}`
// меняет только кусок, а картинку и девятину берёт у обычного вида.
void ApplyLook(sage::ui::Look& look, const sol::table& t) {
    for (const auto& kv : t) {
        if (!kv.first.is<std::string>()) continue;
        const std::string key = kv.first.as<std::string>();
        static const char* const known[] = {"color", "image", "sprite", "slice", "pixel", "fit", "sharp",
                                            "rounding", "border", "border_color", "gradient", "shadow",
                                            "tile_edges", "center"};
        bool ok = false;
        for (const char* k : known) ok = ok || key == k;
        if (!ok) throw std::runtime_error("ui: неизвестный ключ вида '" + key + "'");
    }
    if (t["image"].valid()) {
        look.Texture = t.get<std::string>("image");
        // Картинки интерфейса почти всегда пиксельные (наборы, листы): резкая
        // фильтрация по умолчанию. Фото и плавные градиенты — sharp = false.
        look.Filtering = sage::TextureFiltering::Nearest;
    }
    if (t["sharp"].valid())
        look.Filtering = t.get<bool>("sharp") ? sage::TextureFiltering::Nearest : sage::TextureFiltering::Smooth;
    if (t["color"].valid()) look.Color = ColorArg(t["color"], "color");
    if (t["fit"].valid()) {
        const std::string f = t.get<std::string>("fit");
        int mode = -1;
        for (int i = 0; i < 5; ++i)
            if (f == kFits[i]) mode = i;
        if (mode < 0) throw std::runtime_error("ui: fit '" + f + "' — нужно stretch, nineslice, tile, fit или cover");
        look.Fit = mode;
    }
    if (t["sprite"].valid()) {
        sol::object s = t["sprite"];
        if (s.is<std::string>()) {
            if (look.Texture.empty()) throw std::runtime_error("ui: sprite по имени — без image");
            const sage::ui::SpriteSheet* sheet = SheetFor(look.Texture);
            const sage::ui::SpriteRegion* r = sheet ? sheet->Find(s.as<std::string>()) : nullptr;
            if (!r)
                throw std::runtime_error("ui: в листе '" + look.Texture + "' нет куска '" + s.as<std::string>() + "'");
            look.Sprite = glm::vec4(r->Rect);
            // Кусок-рамка из листа ложится девятиной, если её не задали явно.
            if (r->Slice != glm::vec4(0.0f) && !t["slice"].valid()) {
                look.SliceBorder = r->Slice;
                if (!t["fit"].valid()) look.Fit = 1;
            }
        } else {
            look.Sprite = Vec4Arg(s, "sprite");
        }
    }
    if (t["slice"].valid()) {
        look.SliceBorder = Vec4Arg(t["slice"], "slice");
        if (!t["fit"].valid()) look.Fit = 1;
    }
    if (t["tile_edges"].valid()) {
        const auto fill = t.get<bool>("tile_edges") ? sage::ui::SliceFill::Tile : sage::ui::SliceFill::Stretch;
        look.SliceEdgeFill = fill;
        look.SliceCenterFill = fill;
    }
    if (t["center"].valid()) look.SliceDrawCenter = t.get<bool>("center");
    if (t["pixel"].valid()) look.PixelScale = t.get<float>("pixel");
    if (t["rounding"].valid()) look.Rounding = t.get<float>("rounding");
    if (t["border"].valid()) look.BorderThickness = t.get<float>("border");
    if (t["border_color"].valid()) look.BorderColor = ColorArg(t["border_color"], "border_color");
    if (t["gradient"].valid()) look.Gradient = ColorArg(t["gradient"], "gradient");
    if (t["shadow"].valid()) look.ShadowSize = t.get<float>("shadow");
}

} // namespace

namespace {
// Слой на Lua. Строки «---» — аннотации LuaLS для подсказки редактора кода
// (scripts/gen_script_api.py собирает их так же, как у ScriptApi_ProcGen.cpp).
const char* kUIKit = R"LUA(
local U = sage.ui
local Vec2, Vec4 = Vec2, Vec4
local setmetatable, getmetatable, type, rawget, rawset, error, tostring, pairs, ipairs =
    setmetatable, getmetatable, type, rawget, rawset, error, tostring, pairs, ipairs

local ANCHORS = {
    top_left = UIAnchor.TopLeft, top = UIAnchor.TopCenter, top_right = UIAnchor.TopRight,
    left = UIAnchor.CenterLeft, center = UIAnchor.Center, right = UIAnchor.CenterRight,
    bottom_left = UIAnchor.BottomLeft, bottom = UIAnchor.BottomCenter, bottom_right = UIAnchor.BottomRight,
}
local ANCHOR_NAMES = {}
for k, v in pairs(ANCHORS) do ANCHOR_NAMES[v] = k end
local ALIGN = {left = "start", center = "center", right = "end", top = "start", bottom = "end"}
local ALIGN_BACK = {start = "left", center = "center", ["end"] = "right"}
local VALIGN_BACK = {start = "top", center = "center", ["end"] = "bottom"}

local function color(v)
    if type(v) == "table" and getmetatable(v) == nil then return Vec4(v[1] or 1, v[2] or 1, v[3] or 1, v[4] or 1) end
    return v
end

-- Четыре числа (поля л, в, п, н): число — со всех сторон, таблица — по порядку.
local function vec4(v)
    if type(v) == "number" then return Vec4(v, v, v, v) end
    if type(v) == "table" and getmetatable(v) == nil then return Vec4(v[1] or 0, v[2] or 0, v[3] or 0, v[4] or 0) end
    return v
end

-- Свойства элемента: чтение и запись. Одно имя — одно значение, везде.
local get, set = {}, {}

get.x = function(s) return s.e.Position.x end
set.x = function(s, v) local p = s.e.Position; s.e.Position = Vec2(v, p.y) end
get.y = function(s) return s.e.Position.y end
set.y = function(s, v) local p = s.e.Position; s.e.Position = Vec2(p.x, v) end
get.w = function(s) return s.e.Size.x end
set.w = function(s, v) local p = s.e.Size; s.e.Size = Vec2(v, p.y) end
get.h = function(s) return s.e.Size.y end
set.h = function(s, v) local p = s.e.Size; s.e.Size = Vec2(p.x, v) end
get.anchor = function(s) return ANCHOR_NAMES[s.e.Anchor] end
set.anchor = function(s, v)
    local a = ANCHORS[v]
    if not a then error("ui: anchor '" .. tostring(v) .. "' — нужно top_left, top, top_right, left, center, right, bottom_left, bottom или bottom_right", 3) end
    s.e.Anchor = a
end
get.stretch = function(s) local m = s.e.Stretch
    return m == UIStretch.Both and "both" or m == UIStretch.Horizontal and "x" or m == UIStretch.Vertical and "y" or "none" end
set.stretch = function(s, v)
    local m = (v == true or v == "both") and UIStretch.Both or v == "x" and UIStretch.Horizontal
        or v == "y" and UIStretch.Vertical or UIStretch.None
    s.e.Stretch = m
end
get.margin = function(s) return s.e.Margin end
set.margin = function(s, v) s.e.Margin = vec4(v) end
-- Спрятанный элемент исчезает вместе с детьми и не ловит мышь.
get.visible = function(s) return s.e.Active end
set.visible = function(s, v) s.e.Active = v and true or false end
get.alpha = function(s) return s.e.Alpha end
set.alpha = function(s, v) s.e.Alpha = v end
get.order = function(s) return s.e.Order end
set.order = function(s, v) s.e.Order = v end
-- Слой поверх всего интерфейса (0 — как у родителя): подсказка из глубины
-- окна выше соседних окон.
get.layer = function(s) return s.e.DrawLayer end
set.layer = function(s, v) s.e.DrawLayer = v end
-- Маска детей: "rect", "rounded" или таблица (см. ui.SetMask); false — снять.
get.mask = function(s) return rawget(s, "_mask") end
set.mask = function(s, v) rawset(s, "_mask", v); U.SetMask(s.obj, v) end
get.rotation = function(s) return s.e.Rotation end
set.rotation = function(s, v) s.e.Rotation = v end
get.color = function(s) return s.e.Color end
set.color = function(s, v) s.e.Color = color(v) end
get.text = function(s) local t = rawget(s, "label") or s.e; return t.Text end
set.text = function(s, v) local t = rawget(s, "label") or s.e; t.Text = tostring(v) end
get.text_color = function(s) local t = rawget(s, "label") or s.e; return t.TextColor end
set.text_color = function(s, v)
    local t = rawget(s, "label") or s.e
    v = color(v)
    t.TextColor = v
    -- Тень — тем же цветом вчетверо темнее (как у пиксельных наборов), если
    -- её не задали своим цветом.
    if rawget(s, "auto_shadow") then t.TextShadowColor = Vec4(v.x * 0.25, v.y * 0.25, v.z * 0.25, v.w) end
end
-- Поле от края элемента до текста (слева и справа).
get.pad = function(s) return (rawget(s, "label") or s.e).PadX end
set.pad = function(s, v) (rawget(s, "label") or s.e).PadX = v end
get.shadow_color = function(s) return (rawget(s, "label") or s.e).TextShadowColor end
set.shadow_color = function(s, v) rawset(s, "auto_shadow", nil); (rawget(s, "label") or s.e).TextShadowColor = color(v) end
-- Как элемент ложится на то, что под ним: "normal", "add", "multiply",
-- "screen", "invert" (прицел, видный на любом фоне).
get.blend = function(s) return s.e.Blend end
set.blend = function(s, v) s.e.Blend = v end
get.image = function(s) return rawget(s, "_image") end
set.image = function(s, v) rawset(s, "_image", v); U.SetLook(s.obj, "fill", {image = v}) end
get.sprite = function(s) return rawget(s, "_sprite") end
set.sprite = function(s, v) rawset(s, "_sprite", v); U.SetLook(s.obj, "fill", {sprite = v}) end
get.value = function(s) return U.GetValue(s.obj) end
set.value = function(s, v) U.SetValue(s.obj, v) end
get.enabled = function(s) return s.e.Enabled end
set.enabled = function(s, v) s.e.Enabled = v and true or false end
get.focused = function(s) return s.e.Focused end
set.focused = function(s, v) s.e.Focused = v and true or false end
get.placeholder = function(s) return s.e.Placeholder end
set.placeholder = function(s, v) s.e.Placeholder = v end
-- Состояние мыши — только чтение: его пишет ввод движка.
get.hovered = function(s) return s.e.Hovered end
get.pressed = function(s) return s.e.Pressed end
get.clicked = function(s) return s.e.Clicked end
get.changed = function(s) return s.e.Changed end
-- Настоящий размер после раскладки (растяжение, авто-ширина) — Vec2.
get.real_size = function(s) return s.e.ResolvedSize end
-- Высота строки текста в пикселях (как ключ size у строителя).
get.size = function(s) return (rawget(s, "label") or s.e).TextScale * 8 end
set.size = function(s, v) (rawget(s, "label") or s.e).TextScale = v / 8 end
get.on_click = function(s) return rawget(s, "_on_click") end
set.on_click = function(s, fn)
    local old = rawget(s, "_click_conn")
    if old then sage.events.Off(old); rawset(s, "_click_conn", nil) end
    rawset(s, "_on_click", fn)
    if fn then rawset(s, "_click_conn", U.Connect(s.obj, "clicked", function() fn(s) end)) end
end
get.on_change = function(s) return rawget(s, "_on_change") end
set.on_change = function(s, fn)
    local old = rawget(s, "_change_conn")
    if old then sage.events.Off(old); rawset(s, "_change_conn", nil) end
    rawset(s, "_on_change", fn)
    if fn then rawset(s, "_change_conn", U.Connect(s.obj, "changed", function() fn(s, U.GetValue(s.obj)) end)) end
end

local methods = {}
local function newindex(s, k, v)
    local f = set[k]
    if not f then error("ui: у элемента нет свойства '" .. tostring(k) .. "' (или оно только для чтения)", 2) end
    f(s, v)
end

-- Поставить элемент: x, y — от якоря внутрь родителя; w, h — размер.
function methods.place(s, x, y, w, h)
    if type(s) ~= "table" or not rawget(s, "obj") then error("ui: place — через точку: el.place(x, y, w, h)", 2) end
    s.e.Position = Vec2(x, y)
    if w then s.e.Size = Vec2(w, h or s.e.Size.y) end
end
function methods.show(s) s.e.Active = true end
function methods.hide(s) s.e.Active = false end
function methods.remove(s) s.obj:Destroy() end
-- Вид части: "fill", "hover", "pressed", "disabled", "focused", "track",
-- "filled", "knob", "check", "bar". Таблица — как у строителей.
function methods.look(s, part, t) U.SetLook(s.obj, part, t) end

-- Методы зовутся через точку: el.place(...). Замыкание на элемент — одно на
-- метод при первом обращении.
local function bind(s)
    local bound = {}
    for k, f in pairs(methods) do bound[k] = function(...) return f(s, ...) end end
    return bound
end

local COMMON = {parent = 1, name = 1, x = 1, y = 1, w = 1, h = 1, anchor = 1, stretch = 1, margin = 1,
    visible = 1, alpha = 1, order = 1, rotation = 1, blend = 1, layer = 1, mask = 1, on_click = 1, on_change = 1}

local function wrap(obj, kind, label)
    local s = {obj = obj, e = obj:GetUI(), kind = kind, label = label}
    local bound = bind(s)
    return setmetatable(s, {
        __index = function(t, k)
            local m = bound[k]
            if m then return m end
            local g = get[k]
            if g then return g(t) end
            error("ui: у элемента нет свойства '" .. tostring(k) .. "'", 2)
        end,
        __newindex = newindex,
        __tostring = function() return "ui." .. kind .. " '" .. obj.Name .. "'" end,
    })
end

local function new(kind, o, extra)
    o = o or {}
    for k in pairs(o) do
        if not COMMON[k] and not (extra and extra[k]) then
            error("ui." .. kind .. ": неизвестный ключ '" .. tostring(k) .. "'", 3)
        end
    end
    local obj = SpawnObject(o.name or kind)
    local e = obj:AddUI()
    local p = o.parent
    if p then obj:SetParent(type(p) == "table" and p.obj or p) end
    e.Anchor = UIAnchor.TopLeft
    e.Pivot = Vec2(0, 0)
    e.Position = Vec2(o.x or 0, o.y or 0)
    e.Size = Vec2(o.w or 100, o.h or 20)
    return obj, e
end

local function finish(s, o)
    if o.anchor then s.anchor = o.anchor end
    if o.stretch then s.stretch = o.stretch end
    if o.margin then s.margin = o.margin end
    if o.alpha then s.alpha = o.alpha end
    if o.order then s.order = o.order end
    if o.rotation then s.rotation = o.rotation end
    if o.blend then s.blend = o.blend end
    if o.layer then s.layer = o.layer end
    if o.mask ~= nil then s.mask = o.mask end
    if o.visible == false then s.visible = false end
    if o.on_click then s.on_click = o.on_click end
    if o.on_change then s.on_change = o.on_change end
    return s
end

-- Ключи вида — у панели, картинки, кнопки, поля.
local LOOK = {color = 1, image = 1, sprite = 1, slice = 1, pixel = 1, fit = 1, sharp = 1, rounding = 1,
    border = 1, border_color = 1, gradient = 1, shadow = 1, tile_edges = 1, center = 1}
local function look_of(o, defaults)
    local t = {}
    for k, v in pairs(defaults or {}) do t[k] = v end
    -- С картинкой цвет — её оттенок: без своего цвета картинка рисуется
    -- как есть, а не тонированной в цвет плашки по умолчанию.
    if o.image ~= nil then t.color = Vec4(1, 1, 1, 1) end
    for k in pairs(LOOK) do if o[k] ~= nil then t[k] = o[k] end end
    return t
end
local function merge(a, b) local t = {} for k, v in pairs(a or {}) do t[k] = v end for k, v in pairs(b or {}) do t[k] = v end return t end

local TEXT = {text = 1, font = 1, size = 1, text_color = 1, align = 1, valign = 1, shadow_color = 1,
    shadow_offset = 1, wrap = 1, pad = 1, bold = 1, italic = 1}
local function setup_text(e, o, s)
    e.Text = o.text and tostring(o.text) or ""
    if o.font then
        e.Font = o.font
        -- Шрифт-картинка и пиксельный шрифт — ближайшим соседом и целым шагом
        -- пикселя: дробный масштаб выбрасывает строки пикселей букв.
        if o.font:match("%.png$") then e.FontFilter = "nearest"; e.FontSnapPixels = true end
    end
    if o.size then e.TextScale = o.size / 8 end
    e.TextAlign = ALIGN[o.align or "left"] or "start"
    e.TextVAlign = ALIGN[o.valign or "center"] or "center"
    e.PadX = o.pad or 0
    if o.wrap then e.WrapText = true end
    if o.bold then e.Bold = true end
    if o.italic then e.Italic = true end
    local c = color(o.text_color or Vec4(1, 1, 1, 1))
    e.TextColor = c
    if o.shadow_offset then e.TextShadowOffset = type(o.shadow_offset) == "number"
        and Vec2(o.shadow_offset, o.shadow_offset) or o.shadow_offset end
    if o.shadow_color then e.TextShadowColor = color(o.shadow_color)
    elseif o.shadow_offset then rawset(s, "auto_shadow", true); e.TextShadowColor = Vec4(c.x * 0.25, c.y * 0.25, c.z * 0.25, c.w) end
end
get.align = function(s) return ALIGN_BACK[(rawget(s, "label") or s.e).TextAlign] end
set.align = function(s, v) (rawget(s, "label") or s.e).TextAlign = ALIGN[v] or error("ui: align — left, center или right", 3) end
get.valign = function(s) return VALIGN_BACK[(rawget(s, "label") or s.e).TextVAlign] end
set.valign = function(s, v) (rawget(s, "label") or s.e).TextVAlign = ALIGN[v] or error("ui: valign — top, center или bottom", 3) end

local function keys(...) local t = {} for _, tbl in ipairs({...}) do for k in pairs(tbl) do t[k] = 1 end end return t end

---Экран: элемент во весь экран, корень интерфейса. order — порядок между
---экранами (больше — поверх), input = false — показывается, но не ловит мышь.
---scale = "pixel" — целый масштаб под опорный размер reference {w, h} (для
---пиксельной графики: на любом разрешении пиксели одинаковые), "fit" —
---плавный; max — предел целого масштаба. Без scale — единица = пиксель.
---@param o? table
---@return table
function U.screen(o)
    o = o or {}
    local obj, e = new("screen", o, {input = 1, scale = 1, reference = 1, max = 1})
    e.Stretch = UIStretch.Both
    e.Position = Vec2(0, 0)
    if o.scale ~= nil and o.scale ~= "pixel" and o.scale ~= "fit" then
        error("ui.screen: scale '" .. tostring(o.scale) .. "' — нужно pixel или fit", 2)
    end
    local ref = o.reference
    if type(ref) == "table" and getmetatable(ref) == nil then ref = Vec2(ref[1], ref[2]) end
    SetUICanvas(obj, {order = o.order or 0, pixelScale = o.scale == "pixel", scale = o.scale == "fit",
                      reference = ref, maxScale = o.max})
    if o.input == false then e.BlockRaycasts = false end
    return finish(wrap(obj, "screen"), merge(o, {order = false, scale = false}))
end

---Панель: прямоугольник цветом или картинкой (девятина, плитка).
---@param o table
---@return table
function U.panel(o)
    local obj = new("panel", o, LOOK)
    U.SetLook(obj, "fill", look_of(o, {color = Vec4(0, 0, 0, 0.5), rounding = 0}))
    return finish(wrap(obj, "panel"), o)
end

---Картинка: файл или кусок листа (sprite = "имя" из .sagesheet или {x, y, w, h}).
---@param o table
---@return table
function U.image(o)
    local obj = new("image", o, LOOK)
    U.SetLook(obj, "fill", look_of(o, {color = Vec4(1, 1, 1, 1), rounding = 0}))
    local s = wrap(obj, "image")
    rawset(s, "_image", o.image)
    rawset(s, "_sprite", o.sprite)
    return finish(s, o)
end

---Надпись. size — высота строки в пикселях, align — left/center/right,
---valign — top/center/bottom; shadow_offset — тень (цвет вчетверо темнее).
---@param o table
---@return table
function U.text(o)
    local obj, e = new("text", o, TEXT)
    local s = wrap(obj, "text")
    setup_text(e, o, s)
    return finish(s, o)
end

---Кнопка: вид (как у панели) и вид в состояниях hover/pressed/disabled —
---таблицами с теми же ключами; надпись — text и ключи надписи; цвет надписи в
---состояниях — hover_text_color, disabled_text_color. on_click = function(btn).
---@param o table
---@return table
function U.button(o)
    local obj, e = new("button", o, keys(LOOK, TEXT, {hover = 1, pressed = 1, disabled = 1,
        hover_text_color = 1, pressed_text_color = 1, disabled_text_color = 1, offset = 1, text_margin = 1}))
    local base = look_of(o, {color = Vec4(0.2, 0.22, 0.27, 1), rounding = 0})
    U.SetLook(obj, "fill", base)
    e.Interactive = true
    -- Состояния без своей картинки — тот же вид другим цветом/куском.
    if o.hover then U.SetLook(obj, "hover", merge(base, o.hover)) end
    if o.pressed then U.SetLook(obj, "pressed", merge(base, o.pressed)) end
    if o.disabled then U.SetLook(obj, "disabled", merge(base, o.disabled)) end
    local lo = SpawnObject((o.name or "button") .. " Text")
    local le = lo:AddUI()
    lo:SetParent(obj)
    le.Anchor = UIAnchor.TopLeft
    le.Stretch = UIStretch.Both
    le.Position = Vec2(0, 0)
    if o.text_margin then le.Margin = vec4(o.text_margin) end
    local s = wrap(obj, "button", le)
    setup_text(le, merge({align = "center"}, o), s)
    U.SetTextColors(lo, {hover = o.hover_text_color and color(o.hover_text_color),
        pressed = o.pressed_text_color and color(o.pressed_text_color),
        disabled = o.disabled_text_color and color(o.disabled_text_color)})
    if o.offset then U.SetPressedOffset(obj, o.offset) end
    return finish(s, o)
end

---Поле ввода: text, max (длина), placeholder, password; вид — как у панели,
---focused — вид, пока в поле печатают.
---@param o table
---@return table
function U.field(o)
    local obj, e = new("field", o, keys(LOOK, TEXT, {max = 1, placeholder = 1, password = 1, focused = 1}))
    e.Type = UIKind.Input
    U.SetLook(obj, "fill", look_of(o, {color = Vec4(0, 0, 0, 1), rounding = 0}))
    if o.focused then U.SetLook(obj, "focused", merge(look_of(o, {}), o.focused)) end
    local s = wrap(obj, "field")
    setup_text(e, merge({pad = 4}, o), s)
    e.MaxLength = o.max or 0
    if o.placeholder then e.Placeholder = o.placeholder end
    if o.password then e.Password = true end
    return finish(s, o)
end

---Ползунок: value, min, max, step; вид частей — track, filled, knob
---(таблицы вида). on_change = function(slider, value).
---@param o table
---@return table
function U.slider(o)
    local obj, e = new("slider", o, {value = 1, min = 1, max = 1, step = 1, track = 1, filled = 1, knob = 1})
    e.Type = UIKind.Slider
    e.MinValue = o.min or 0
    e.MaxValue = o.max or 1
    if o.track then U.SetLook(obj, "track", o.track) end
    if o.filled then U.SetLook(obj, "filled", o.filled) end
    if o.knob then U.SetLook(obj, "knob", o.knob) end
    if o.step then U.SetRangeStep(obj, o.step) end
    U.SetValue(obj, o.value or e.MinValue)
    return finish(wrap(obj, "slider"), o)
end

---Полоса (здоровье, загрузка): value 0..1; вид фона — как у панели,
---заполнения — fill = {...}.
---@param o table
---@return table
function U.bar(o)
    local obj, e = new("bar", o, keys(LOOK, {value = 1, fill = 1}))
    e.Type = UIKind.Bar
    U.SetLook(obj, "fill", look_of(o, {color = Vec4(0, 0, 0, 0.5), rounding = 0}))
    U.SetLook(obj, "bar", merge({color = Vec4(0.4, 0.8, 0.4, 1), rounding = 0}, o.fill))
    e.Value = o.value or 1
    return finish(wrap(obj, "bar"), o)
end

---Группа: невидимый контейнер. layout = "row" | "column" | "grid" расставляет
---детей сам: gap — зазор, padding — поля {л, в, п, н} или число, columns,
---justify = start | center | end | between | around | evenly, cross. У детей
---работает margin — свои поля вокруг. clip = true обрезает детей.
---@param o? table
---@return table
function U.group(o)
    o = o or {}
    local obj, e = new("group", o, {layout = 1, gap = 1, padding = 1, columns = 1, justify = 1, cross = 1, clip = 1})
    e.Type = UIKind.Panel
    e.Color = Vec4(0, 0, 0, 0)
    if o.layout then
        SetUILayout(obj, {dir = o.layout, spacing = o.gap or 0, padding = vec4(o.padding or 0),
            columns = o.columns or 0, justify = o.justify or "start", cross = o.cross or "start"})
    end
    if o.clip then e.ClipChildren = true end
    return finish(wrap(obj, "group"), o)
end

---Курсор мыши в пикселях интерфейса.
---@return Vec2
function U.cursor() return UICursor() end

---Размер экрана в пикселях интерфейса.
---@return Vec2
function U.size() return UIScreenSize() end

---Зажата ли левая кнопка мыши.
---@return boolean
function U.mouse_down() return UIMouseDown() end
)LUA";
} // namespace

void ScriptEngine::RegisterUIKitApi() {
    // Вид части элемента из таблицы — ОДИН путь для подложки, состояний кнопки,
    // частей ползунка и полосы. false/nil вместо таблицы у состояния — снять.
    Bind("ui", "SetLook", nullptr, [](GameObject& obj, const std::string& part, sol::object value) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity()))
            throw std::runtime_error("ui.SetLook: объект не элемент интерфейса");
        entt::registry& reg = *obj.Registry();
        const entt::entity e = obj.Entity();
        const bool off = !value.is<sol::table>();
        sol::table t = off ? sol::table() : value.as<sol::table>();
        auto state = [&](bool sage::ui::Interactable::*use, sage::ui::Look sage::ui::Interactable::*look) {
            sage::ui::Interactable& act = reg.get_or_emplace<sage::ui::Interactable>(e);
            act.*use = !off;
            if (off) return;
            // Состояние начинается с обычного вида: так `hover = {sprite = ...}`
            // меняет один кусок, а не обнуляет картинку и девятину.
            if (const sage::ui::Fill* f = reg.try_get<sage::ui::Fill>(e)) act.*look = *f;
            ApplyLook(act.*look, t);
        };
        if (part == "fill") {
            if (off) { reg.remove<sage::ui::Fill>(e); return; }
            ApplyLook(reg.get_or_emplace<sage::ui::Fill>(e), t);
        } else if (part == "hover") {
            state(&sage::ui::Interactable::UseHoverLook, &sage::ui::Interactable::HoverLook);
        } else if (part == "pressed") {
            state(&sage::ui::Interactable::UsePressedLook, &sage::ui::Interactable::PressedLook);
        } else if (part == "disabled") {
            state(&sage::ui::Interactable::UseDisabledLook, &sage::ui::Interactable::DisabledLook);
        } else if (part == "focused") {
            state(&sage::ui::Interactable::UseFocusedLook, &sage::ui::Interactable::FocusedLook);
        } else if (part == "track" || part == "filled" || part == "knob" || part == "check") {
            if (off) return;
            sage::ui::Range& r = reg.get_or_emplace<sage::ui::Range>(e);
            ApplyLook(part == "track" ? r.Track : part == "filled" ? r.Filled : part == "knob" ? r.Knob : r.Check, t);
        } else if (part == "bar") {
            if (off) return;
            ApplyLook(reg.get_or_emplace<sage::ui::Bar>(e).Filled, t);
        } else {
            throw std::runtime_error("ui.SetLook: часть '" + part +
                                     "' — нужно fill, hover, pressed, disabled, focused, track, filled, knob, check или bar");
        }
    });

    // Маска поддерева: "rect" | "rounded" | таблица {shape = "rect" | "rounded" |
    // "image", rounding, image, sprite, feather, invert, padding, sharp};
    // false/nil — снять. Прямоугольник режут ножницы, остальное — шейдер.
    Bind("ui", "SetMask", nullptr, [](GameObject& obj, sol::object value) {
        if (!obj.Valid() || !sage::ui::IsElement(*obj.Registry(), obj.Entity()))
            throw std::runtime_error("ui.SetMask: объект не элемент интерфейса");
        entt::registry& reg = *obj.Registry();
        if (!value.valid() || value.get_type() == sol::type::lua_nil ||
            (value.get_type() == sol::type::boolean && !value.as<bool>())) {
            reg.remove<sage::ui::Mask>(obj.Entity());
            return;
        }
        sage::ui::Mask m;
        auto shape = [&](const std::string& n) {
            if (n == "rect") m.Form = sage::ui::Mask::Shape::Rect;
            else if (n == "rounded") m.Form = sage::ui::Mask::Shape::RoundedRect;
            else if (n == "image") m.Form = sage::ui::Mask::Shape::Image;
            else throw std::runtime_error("ui.mask: форма '" + n + "' — нужно rect, rounded или image");
        };
        if (value.is<std::string>()) {
            shape(value.as<std::string>());
        } else if (value.is<sol::table>() || value.get_type() == sol::type::boolean) {
            if (value.is<sol::table>()) {
                sol::table t = value.as<sol::table>();
                for (const auto& kv : t) {
                    const std::string k = kv.first.as<std::string>();
                    static const char* const known[] = {"shape", "rounding", "image", "sprite", "feather",
                                                        "invert", "padding", "sharp"};
                    bool ok = false;
                    for (const char* n : known) ok = ok || k == n;
                    if (!ok) throw std::runtime_error("ui.mask: неизвестный ключ '" + k + "'");
                }
                if (t["image"].valid()) {
                    m.Form = sage::ui::Mask::Shape::Image;
                    m.Image = t.get<std::string>("image");
                }
                if (t["shape"].valid()) shape(t.get<std::string>("shape"));
                if (t["rounding"].valid()) {
                    m.Rounding = t.get<float>("rounding");
                    if (!t["shape"].valid() && m.Form == sage::ui::Mask::Shape::Rect)
                        m.Form = sage::ui::Mask::Shape::RoundedRect;
                }
                if (t["sprite"].valid()) m.Sprite = Vec4Arg(t["sprite"], "sprite");
                if (t["feather"].valid()) m.Feather = t.get<float>("feather");
                if (t["invert"].valid()) m.Invert = t.get<bool>("invert");
                if (t["padding"].valid()) m.Padding = Vec4Arg(t["padding"], "padding");
                if (t["sharp"].valid() && t.get<bool>("sharp")) m.Filtering = sage::TextureFiltering::Nearest;
            }
        } else {
            throw std::runtime_error("ui.mask: ожидается имя формы или таблица");
        }
        reg.emplace_or_replace<sage::ui::Mask>(obj.Entity(), m);
    });

    // Цвет надписи в состояниях кнопки (наведение, нажатие, недоступна).
    // Состояние берётся у ближайшего реагирующего предка — надпись кнопки
    // светлеет вместе с ней, без опроса скриптом каждый кадр.
    Bind("ui", "SetTextColors", nullptr, [](GameObject& obj, sol::table t) {
        if (!obj.Valid()) return;
        sage::ui::Label& l = obj.Registry()->get_or_emplace<sage::ui::Label>(obj.Entity());
        bool any = false;
        if (t["hover"].valid()) { l.HoverColor = ColorArg(t["hover"], "hover"); any = true; }
        else l.HoverColor = l.Color;
        if (t["pressed"].valid()) { l.PressedColor = ColorArg(t["pressed"], "pressed"); any = true; }
        else l.PressedColor = t["hover"].valid() ? l.HoverColor : l.Color;
        if (t["disabled"].valid()) { l.DisabledColor = ColorArg(t["disabled"], "disabled"); any = true; }
        else l.DisabledColor = l.Color;
        l.StateColors = any;
    });

    // Сдвиг содержимого нажатой кнопки (надпись «проваливается»).
    Bind("ui", "SetPressedOffset", nullptr, [](GameObject& obj, sol::object v) {
        if (!obj.Valid()) return;
        glm::vec2 off = v.is<glm::vec2>() ? v.as<glm::vec2>() : glm::vec2(0.0f, v.as<float>());
        obj.Registry()->get_or_emplace<sage::ui::Interactable>(obj.Entity()).PressedOffset = off;
    });

    // Шаг ползунка (громкость по 5%): 0 — плавно.
    Bind("ui", "SetRangeStep", nullptr, [](GameObject& obj, float step) {
        if (!obj.Valid()) return;
        obj.Registry()->get_or_emplace<sage::ui::Range>(obj.Entity()).Step = std::max(0.0f, step);
    });

    // Подписка на сигнал элемента (clicked, changed…) функцией Lua. Номер
    // подписки снимается Events.off(номер). Объект уничтожен — подписка
    // уходит вместе с ним.
    Bind("ui", "Connect", nullptr, [this](GameObject& obj, const std::string& signal,
                                          sol::protected_function fn) -> int {
        if (!obj.Valid()) throw std::runtime_error("ui.Connect: объект недействителен");
        const IdComponent* id = obj.Registry()->try_get<IdComponent>(obj.Entity());
        if (!id) throw std::runtime_error("ui.Connect: у объекта нет номера (IdComponent)");
        return m_events->Connect(EventBus(), id->Id, signal, std::move(fn), false, 0);
    });

    sol::protected_function_result r = m_lua.safe_script(kUIKit, sol::script_pass_on_error, "uikit");
    if (!r.valid()) {
        sol::error e = r;
        throw std::runtime_error(std::string("uikit: ") + e.what());
    }
}

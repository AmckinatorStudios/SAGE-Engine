#include "sage/scripting/ScriptEngine.h"

// ---------------------------------------------------------------------------
// ПРОЦЕДУРНАЯ ГЕНЕРАЦИЯ — В СТИЛЕ SAGE (см. docs/scripting.md, «Стиль API»).
//
// Возможности те же, что у sage.noise / sage.math / sage.mesh / sage.material
// / sage.texture (ScriptApi_Procedural.cpp), но пишутся так, как читаются:
//
//      ground = grid(100, 100, 128, 128, function(x, z) return fbm(x * 0.02, z * 0.02) * 12 end)
//      ground.apply(self)
//      stone = material("stone", {color = rgb(0.5, 0.45, 0.4), roughness = 0.9})
//      stone.metallic = 0.1
//
// Правила стиля, по которым это сделано (и которые обязательны для нового API):
//   • без префикса sage. — простые имена в нижнем регистре через «_»;
//   • объект — точка, а не двоеточие: m.vertex(...), а не m:Vertex(...);
//   • свойства — присваиванием (stone.roughness = 0.9), а не Set("roughness");
//   • настройки — одной таблицей {kind = "simplex", octaves = 5}, необязательные
//     аргументы — в конце и с разумным умолчанием;
//   • одно действие — одна строка, без цепочек вызовов.
//
// Слой написан на Lua поверх привязок C++: здесь нет новой математики, только
// имена и форма вызова. Цена — одно замыкание на метод объекта при создании;
// горячие функции (perlin, fbm, lerp…) — прямые ссылки на функции C++.
//
// Строки «---» — аннотации LuaLS: из них scripts/gen_script_api.py собирает
// подсказку редактора кода (editor/assets/api/sage.lua).
// ---------------------------------------------------------------------------

namespace {

const char* kProcGen = R"LUA(
local N, M, MESH, MAT, TEX = sage.noise, sage.math, sage.mesh, sage.material, sage.texture
local Vec2, Vec3 = Vec2, Vec3
local setmetatable, type = setmetatable, type

-- Точка из чисел или готовый Vec3.
local function vec(x, y, z)
    if type(x) == "number" then return Vec3(x, y or 0, z or 0) end
    return x
end

-- ---------------------------------------------------------------- цвет

---Цвет из трёх долей 0..1.
---@param r number
---@param g number
---@param b number
---@return Vec3
function rgb(r, g, b) return Vec3(r, g, b) end

---Серый цвет яркости v (0..1).
---@param v number
---@return Vec3
function gray(v) return Vec3(v, v, v) end

-- ---------------------------------------------------------------- шум

---Шум Перлина в [-1, 1]. z можно не давать.
---@param x number
---@param y? number
---@param z? number
---@return number
function perlin(x, y, z) return N.Perlin(x, y or 0, z) end

---Симплекс-шум в [-1, 1].
---@param x number
---@param y? number
---@param z? number
---@return number
function simplex(x, y, z) return N.Simplex(x, y or 0, z) end

---Value-шум в [-1, 1]: мягче, «облачнее» градиентного.
---@param x number
---@param y? number
---@param z? number
---@return number
function value_noise(x, y, z) return N.Value(x, y or 0, z) end

---Клеточный шум: расстояние до ближайшей точки, до второй и номер клетки.
---@param x number
---@param y? number
---@param z? number
---@return number f1
---@return number f2
---@return number cell
function worley(x, y, z) return N.Worley(x, y or 0, z) end

---Фрактальный шум (несколько слоёв Перлина) в [-1, 1].
---@param x number
---@param y? number
---@param z? number
---@return number
function fbm(x, y, z) return N.Fbm(x, y or 0, z or 0) end

---Гребни: горные хребты, трещины. В [0, 1].
---@param x number
---@param y? number
---@param z? number
---@return number
function ridged(x, y, z) return N.Ridged(x, y or 0, z or 0) end

---Турбулентность: |шум| по слоям — дым, мрамор. В [0, 1].
---@param x number
---@param y? number
---@param z? number
---@return number
function turbulence(x, y, z) return N.Turbulence(x, y or 0, z or 0) end

---Точка, сдвинутая шумом: искажение для «живых» узоров.
---@param x number
---@param y number
---@param z number
---@param strength? number
---@return Vec3
function warp(x, y, z, strength) return N.Warp(Vec3(x, y, z), strength or 1) end

---Зерно для perlin, simplex, fbm и прочих простых функций шума.
---@param seed integer
function set_seed(seed) N.Seed(seed) end

local NOISE_KEYS = { scale = "frequency", kind = "type" }

-- noise{kind = "simplex", seed = 42, octaves = 5, scale = 0.02} -> генератор.
-- Вызов генератора — фрактальный шум; остальное — методами через точку.
local function make_noise(opts)
    if type(opts) == "string" then opts = { kind = opts } end
    opts = opts or {}
    local raw = {}
    for k, v in pairs(opts) do raw[NOISE_KEYS[k] or k] = v end
    local g = N.New(raw)
    local o = {
        ridged = function(x, y, z) return g:Ridged(x, y or 0, z or 0) end,
        turbulence = function(x, y, z) return g:Turbulence(x, y or 0, z or 0) end,
        raw = function(x, y, z) return g:Sample(x, y, z) end,
        hash = function(x, y, z) return g:Hash(x, y, z) end,
    }
    return setmetatable(o, {
        __call = function(_, x, y, z) return g:Fbm(x, y or 0, z or 0) end,
        __index = function(_, k) return g[NOISE_KEYS[k] or k] end,
        __newindex = function(_, k, v) g[NOISE_KEYS[k] or k] = v end,
    })
end
---Свой генератор шума: noise{kind = "simplex", seed = 42, octaves = 5, scale = 0.02}.
---@class sage.noise
---@overload fun(opts?: table|string): Noise
setmetatable(N, { __call = function(_, opts) return make_noise(opts) end })

---@class Noise
---@field seed integer
---@field octaves integer
---@field scale number
---@field lacunarity number
---@field gain number
---@field ridged fun(x:number, y?:number, z?:number): number
---@field turbulence fun(x:number, y?:number, z?:number): number
---@field raw fun(x:number, y?:number, z?:number): number
---@field hash fun(x:integer, y?:integer, z?:integer): number
---@overload fun(x:number, y?:number, z?:number): number

-- Свой генератор шума: noise{kind = "simplex", seed = 42, octaves = 5, scale = 0.02}
-- (kind: perlin, simplex, value, worley, worley2, cell). noise — это та же
-- таблица, что sage.noise (короткое имя модуля), только вызываемая.

-- ---------------------------------------------------------------- математика

---Число из диапазона [a, b] — в диапазон [c, d].
---@return number
function remap(v, a, b, c, d) return M.Remap(v, a, b, c, d) end
---Где v между a и b: 0 в a, 1 в b.
---@return number
function inverse_lerp(a, b, v) return M.InverseLerp(a, b, v) end
---Между a и b по доле t.
---@return number
function lerp(a, b, t) return M.Lerp(a, b, t) end
---x в границах [lo, hi].
---@return number
function clamp(x, lo, hi) return M.Clamp(x, lo, hi) end
---Дробная часть.
---@return number
function fract(x) return M.Fract(x) end
---x, замкнутый по кругу в [lo, hi).
---@return number
function wrap(x, lo, hi) return M.Wrap(x, lo, hi) end
---Туда-обратно в [0, length].
---@return number
function ping_pong(t, length) return M.PingPong(t, length) end
---0 до порога, 1 после.
---@return number
function step(edge, x) return M.Step(edge, x) end
---Плавная ступенька между e0 и e1.
---@return number
function smoothstep(e0, e1, x) return M.SmoothStep(e0, e1, x) end
---Ещё более плавная ступенька.
---@return number
function smootherstep(e0, e1, x) return M.SmootherStep(e0, e1, x) end
---Детерминированное «случайное» число 0..1 из целых координат.
---@return number
function hash(x, y, z, seed) return M.Hash(x, y, z, seed) end

---@class Random
---@field float fun(): number
---@field range fun(a:number, b:number): number
---@field int fun(a:integer, b:integer): integer
---@field chance fun(p:number): boolean
---@field pick fun(list:table): any
---@field seed fun(seed:integer)

---Свой генератор случайных чисел: одно зерно — одна последовательность.
---@param seed? integer
---@return Random
function random(seed)
    local r = M.Rng(seed or 0)
    return {
        float = function() return r:Float() end,
        range = function(a, b) return r:Range(a, b) end,
        int = function(a, b) return r:Int(a, b) end,
        chance = function(p) return r:Chance(p) end,
        pick = function(list) return list[r:Int(1, #list)] end,
        seed = function(s) r:Seed(s) end,
    }
end

---Формула из строки — обычная функция: formula("sin(x) * y", "x, y").
---Внутри видны math, шумы и помощники: sin, clamp, fbm, lerp...
---@param expr string
---@param args? string
---@return function
function formula(expr, args) return M.Formula(expr, args) end

-- ---------------------------------------------------------------- меш

---@class Mesh
---@field vertices integer
---@field triangles integer
---@field vertex fun(x:number|Vec3, y?:number|Vec3, z?:number|Vec2): integer
---@field triangle fun(a:integer, b:integer, c:integer)
---@field quad fun(a:integer, b:integer, c:integer, d:integer)
---@field material fun(slot:integer)
---@field position fun(i:integer): Vec3
---@field set_position fun(i:integer, x:number|Vec3, y?:number, z?:number)
---@field set_normal fun(i:integer, x:number|Vec3, y?:number, z?:number)
---@field set_uv fun(i:integer, u:number, v:number)
---@field normals fun(smooth?:boolean)
---@field deform fun(fn:fun(p:Vec3, n:Vec3, uv:Vec2): Vec3)
---@field move fun(x:number|Vec3, y?:number, z?:number)
---@field scale fun(x:number|Vec3, y?:number, z?:number)
---@field add fun(other:Mesh)
---@field clear fun()
---@field bounds fun(): Vec3, Vec3
---@field apply fun(object:any)
---@field save fun(path:string)

local function wrap_mesh(b)
    local m = {}
    m.vertex = function(x, y, z, normal, uv)
        if type(x) == "number" then return b:Vertex(Vec3(x, y or 0, z or 0), normal, uv) end
        return b:Vertex(x, y, z)   -- vertex(pos, normal, uv)
    end
    m.triangle = function(a, c, d) b:Triangle(a, c, d) end
    m.quad = function(a, c, d, e) b:Quad(a, c, d, e) end
    m.material = function(slot) b:Material(slot) end
    m.position = function(i) return b:GetPosition(i) end
    m.set_position = function(i, x, y, z) b:SetPosition(i, vec(x, y, z)) end
    m.set_normal = function(i, x, y, z) b:SetNormal(i, vec(x, y, z)) end
    m.set_uv = function(i, u, v) b:SetUV(i, Vec2(u, v)) end
    m.normals = function(smooth) b:RecalculateNormals(smooth ~= false) end
    m.deform = function(fn) b:Deform(fn) end
    m.move = function(x, y, z) b:Translate(vec(x, y, z)) end
    m.scale = function(x, y, z)
        if type(x) == "number" and y == nil then b:Scale(x) else b:Scale(vec(x, y, z)) end
    end
    m.add = function(other) b:Append(rawget(other, "_builder") or other) end
    m.clear = function() b:Clear() end
    m.bounds = function() return b:Bounds() end
    m.apply = function(object) b:Apply(object) end
    m.save = function(path) b:Save(path) end
    rawset(m, "_builder", b)
    return setmetatable(m, {
        __index = function(_, k)
            if k == "vertices" then return b:VertexCount() end
            if k == "triangles" then return b:TriangleCount() end
        end,
    })
end
---Пустой меш: вершины, треугольники, материалы — кодом.
---@class sage.mesh
---@overload fun(): Mesh
setmetatable(MESH, { __call = function() return wrap_mesh(MESH.New()) end })

---Плоскость width x depth из cols x rows клеток; height(x, z) — высота точки.
---@param width number
---@param depth? number
---@param cols? integer
---@param rows? integer
---@param height? fun(x:number, z:number): number
---@return Mesh
function grid(width, depth, cols, rows, height)
    cols = cols or 16
    return wrap_mesh(MESH.Grid(width, depth or width, cols, rows or cols, height))
end

---Коробка: одно число — куб, три — размеры по осям.
---@param w number
---@param h? number
---@param d? number
---@return Mesh
function box(w, h, d) return wrap_mesh(MESH.Box(h and Vec3(w, h, d or h) or w)) end

---Сфера радиуса r.
---@param r number
---@param segments? integer
---@param rings? integer
---@return Mesh
function sphere(r, segments, rings) return wrap_mesh(MESH.Sphere(r, segments, rings)) end

---Цилиндр радиуса r и высоты h; caps = false — без крышек.
---@param r number
---@param h number
---@param segments? integer
---@param caps? boolean
---@return Mesh
function cylinder(r, h, segments, caps) return wrap_mesh(MESH.Cylinder(r, h, segments, caps)) end

-- ---------------------------------------------------------------- текстура

---@class Texture
---@field name string
---@field width integer
---@field height integer
---@field nearest boolean
---@field fill fun(fn:fun(u:number, v:number, x:integer, y:integer): Vec3|number)
---@field set fun(x:integer, y:integer, color:Vec3|number)
---@field get fun(x:integer, y:integer): Vec4
---@field apply fun()

local function color_of(c)
    if type(c) == "number" then return Vec3(c, c, c) end
    return c
end

local function wrap_texture(c, name)
    local dirty = true
    local t = {}
    -- На видеокарту — не сразу, а когда текстура понадобится (назначили в
    -- материал или позвали apply): заливку часто переделывают несколько раз,
    -- и грузить каждый промежуточный вариант незачем.
    t.fill = function(fn)
        c:Fill(function(u, v, x, y) return color_of(fn(u, v, x, y)) end)
        dirty = true
    end
    t.set = function(x, y, color) c:SetPixel(x, y, color_of(color)); dirty = true end
    t.get = function(x, y) return c:GetPixel(x, y) end
    t.apply = function() c:Apply(); dirty = false end
    rawset(t, "_flush", function() if dirty then c:Apply(); dirty = false end end)
    return setmetatable(t, {
        __index = function(_, k)
            if k == "name" then return name end
            if k == "width" then return c.width end
            if k == "height" then return c.height end
            if k == "nearest" then return c.nearest end
        end,
        __newindex = function(_, k, v)
            if k ~= "nearest" then error("texture: свойство '" .. tostring(k) .. "' только для чтения", 2) end
            c.nearest = v
            dirty = true
        end,
    })
end
-- Текстура, нарисованная кодом: texture("noise", 256, 256) — вызов таблицы
-- texture (= sage.texture). На видеокарту — при назначении в материал или apply().
---Текстура, нарисованная кодом: texture("noise", 256, 256).
---@class sage.texture
---@overload fun(name: string, width: integer, height?: integer): Texture
setmetatable(TEX, { __call = function(_, name, width, height)
    return wrap_texture(TEX.New(name, width, height or width), name)
end })

-- ---------------------------------------------------------------- материал

---@class Material
---@field name string
---@field color Vec3
---@field emissive Vec3
---@field emissive_strength number
---@field metallic number
---@field roughness number
---@field opacity number
---@field texture string|Texture
---@field normal_map string|Texture
---@field metallic_map string|Texture
---@field roughness_map string|Texture
---@field ao_map string|Texture
---@field emissive_map string|Texture
---@field tiling number|Vec2
---@field unlit boolean
---@field double_sided boolean
---@field cutoff number
---@field nearest boolean
---@field save fun(path:string)

local MAT_KEYS = {
    color = "albedo", albedo = "albedo", emissive = "emissive", emissive_strength = "emissiveStrength",
    metallic = "metallic", roughness = "roughness", opacity = "opacity", texture = "texture",
    normal_map = "normalMap", metallic_map = "metallicMap", roughness_map = "roughnessMap",
    ao_map = "aoMap", emissive_map = "emissiveMap", tiling_x = "tilingX", tiling_y = "tilingY",
    unlit = "unlit", double_sided = "doubleSided", cutoff = "cutoff", nearest = "nearest",
}

local function set_material(name, values, k, v)
    if k == "tiling" then
        local x, y = v, v
        if type(v) ~= "number" then x, y = v.x or v[1], v.y or v[2] end
        MAT.Set(name, "tilingX", x)
        MAT.Set(name, "tilingY", y)
    else
        local key = MAT_KEYS[k]
        if not key then error("material: неизвестное свойство '" .. tostring(k) .. "'", 3) end
        local raw = v
        if type(v) == "table" and rawget(v, "_flush") then   -- текстура кодом
            v._flush()
            raw = v.name
        end
        MAT.Set(name, key, raw)
    end
    values[k] = v
end

-- Материал из кода: material("stone", {color = rgb(0.5, 0.45, 0.4), roughness = 0.9})
-- — вызов таблицы material (= sage.material). Свойства — присваиванием.
local function make_material(name, props)
    MAT.Create(name, {})
    local values = {}
    local h = { save = function(path) MAT.Save(name, path) end }
    setmetatable(h, {
        __index = function(_, k) if k == "name" then return name end return values[k] end,
        __newindex = function(_, k, v) set_material(name, values, k, v) end,
    })
    for k, v in pairs(props or {}) do set_material(name, values, k, v) end
    return h
end
---Материал из кода: material("stone", {color = rgb(0.5, 0.45, 0.4), roughness = 0.9}).
---@class sage.material
---@overload fun(name: string, props?: table): Material
setmetatable(MAT, { __call = function(_, name, props) return make_material(name, props) end })
)LUA";

} // namespace

void ScriptEngine::RegisterProcGenApi() {
    // Глобальные имена ставятся только свободными: игра, у которой уже есть
    // свой `box` или `random`, при своём и остаётся (тот же закон, что у
    // коротких имён модулей). Таблицы mesh/noise/material/texture — те же, что
    // sage.mesh и т.д., только вызываемые: noise{...}, mesh().
    //
    // Поэтому слой исполняется в своём окружении (чтение — из глобальных), а
    // в глобальные переезжает только то, чего там нет.
    sol::table g = m_lua.globals();
    sol::environment env(m_lua, sol::create, g);
    sol::protected_function_result r = m_lua.safe_script(kProcGen, env, sol::script_pass_on_error, "procgen");
    if (!r.valid()) {
        sol::error e = r;
        throw std::runtime_error(std::string("procgen: ") + e.what());
    }
    for (const auto& kv : env) {
        if (kv.first.get_type() != sol::type::string) continue;
        if (g.raw_get<sol::object>(kv.first).get_type() == sol::type::lua_nil) g.raw_set(kv.first, kv.second);
    }
}

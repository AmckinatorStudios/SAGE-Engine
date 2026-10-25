#pragma once

// Части прелюдии SAGE Lua (см. SagePrelude.cpp: PreludeSource() склеивает их
// при компиляции, байт в байт). Разделена по смыслу: модель объектов языка
// отдельно от того, что работает в кадре.
namespace sage::lang {

// Модули, классы и наследование, перечисления, структуры, компоненты, события.
inline constexpr char kPreludeCore[] = R"LUA(
local G = _G
local S = rawget(G, "__sage") or {}
G.__sage = S
S.version = "1.0"
S.modules = S.modules or {}
S.loaded = {}
S.loading = {}
S.current = nil
S.role = S.role or "both"

local unpack = table.unpack or unpack
local function log(level, ...)
    local parts = {}
    for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
    local text = table.concat(parts, " ")
    local D = rawget(G, "Debug")
    if D and D[level] then D[level](text) else print("[" .. level .. "] " .. text) end
end
S.log = log

-- Ошибка в обработчике, таймере или задаче не должна валить кадр: сообщение
-- со стеком — в консоль, игра идёт дальше.
local function report(where, err, co)
    local tb = co and debug and debug.traceback(co, tostring(err)) or
               (debug and debug.traceback(tostring(err), 2) or tostring(err))
    log("Error", where .. ": " .. tb)
end
S.report = report

local function owned(owner)
    -- Владелец жив, пока жив его объект.
    if owner == nil then return true end
    if rawget(owner, "__released") then return false end
    local go = rawget(owner, "gameObject")
    if go ~= nil then
        local ok, valid = pcall(function() return go:IsValid() end)
        if ok and valid == false then return false end
    end
    return true
end

local function call(where, owner, fn, ...)
    local prev = S.current
    S.current = owner
    local ok, err = xpcall(fn, function(e) return debug and debug.traceback(tostring(e), 2) or tostring(e) end, ...)
    S.current = prev
    if not ok then log("Error", where .. ": " .. tostring(err)) end
    return ok
end

-- ===========================================================================
--  Классы
-- ===========================================================================
local Object = {}
Object.__name = "Object"
Object.__isClass = true
Object.__index = Object
Object.__inits = {}
S.Object = Object

function S.classOf(obj)
    if type(obj) ~= "table" then return nil end
    local c = rawget(obj, "__class")
    if c then return c end
    local mt = getmetatable(obj)
    if type(mt) == "table" and rawget(mt, "__isClass") then return mt end
    return nil
end

local function implements(c, T)
    while c do
        local list = rawget(c, "__interfaces")
        if list then
            for _, i in ipairs(list) do
                if i == T then return true end
                local parents = rawget(i, "__parents")
                if parents then
                    for _, p in ipairs(parents) do if p == T then return true end end
                end
            end
        end
        c = rawget(c, "__parent")
    end
    return false
end

function S.is(obj, T)
    local c = S.classOf(obj)
    if c == nil then return false end
    if rawget(T, "__isInterface") then return implements(c, T) end
    while c do
        if c == T then return true end
        c = rawget(c, "__parent")
    end
    return false
end
function Object:is(T) return S.is(self, T) end
function Object:copy()
    local c = S.classOf(self)
    local out = setmetatable({}, c or getmetatable(self))
    for k, v in pairs(self) do rawset(out, k, v) end
    return out
end

local function runInits(obj, C)
    if C == nil then return end
    local parent = rawget(C, "__parent")
    if parent then runInits(obj, parent) end
    local inits = rawget(C, "__inits")
    if inits then
        for _, f in ipairs(inits) do
            if rawget(obj, f[1]) == nil then rawset(obj, f[1], f[2](obj)) end
        end
    end
end
S.runInits = runInits

function S.construct(C, ...)
    local obj = setmetatable({}, C)
    runInits(obj, C)
    local init = C.init
    if init then init(obj, ...) end
    return obj
end

function S.class(name, parent, path)
    if parent == nil then parent = Object end
    if type(parent) ~= "table" then error("класс " .. name .. ": родитель не класс", 2) end
    local C = {__name = name, __parent = parent, __path = path, __inits = {}, __isClass = true}
    C.__index = C
    C.__tostring = function(self)
        local f = self.toString
        if f then return f(self) end
        return name .. ": " .. string.format("%p", self)
    end
    setmetatable(C, {
        __index = parent,
        __call = function(cls, ...) return S.construct(cls, ...) end,
        __tostring = function() return "class " .. name end,
    })
    -- Метаметоды наследуются: Lua ищет их в самой метатаблице, без __index.
    for _, mm in ipairs({"__add", "__sub", "__mul", "__div", "__eq", "__lt", "__le", "__unm", "__concat", "__len"}) do
        local v = rawget(parent, mm)
        if v then C[mm] = v end
    end
    return C
end

function S.field(C, name, initFn)
    local inits = rawget(C, "__inits")
    inits[#inits + 1] = {name, initFn}
end

function S.finish(C, meta)
    meta = meta or {}
    rawset(C, "__meta", meta)
    if meta.interfaces then rawset(C, "__interfaces", meta.interfaces) end
    if meta.kind == "struct" then return C end
    if meta.component then S.component(C) end
    return C
end

function S.interface(name, methods, parents)
    return {__name = name, __isInterface = true, __methods = methods or {}, __parents = parents or {}}
end

-- Структуры: данные с полями по порядку. Vector2(10, 20) или Vector2{ x = 10 }.
-- Значение — таблица (ссылка); копия — v:copy(). Сравнение == — по полям.
function S.struct(name, path)
    local C = {__name = name, __path = path, __fields = {}, __isClass = true, __isStruct = true}
    C.__index = C
    C.__eq = function(a, b)
        for _, f in ipairs(C.__fields) do if a[f[1]] ~= b[f[1]] then return false end end
        return true
    end
    C.__tostring = function(self)
        local parts = {}
        for _, f in ipairs(C.__fields) do parts[#parts + 1] = f[1] .. " = " .. tostring(self[f[1]]) end
        return name .. "(" .. table.concat(parts, ", ") .. ")"
    end
    C.copy = function(self)
        local out = setmetatable({}, C)
        for k, v in pairs(self) do rawset(out, k, v) end
        return out
    end
    C.is = function(self, T) return T == C end
    setmetatable(C, {
        __call = function(cls, ...)
            local obj = setmetatable({}, cls)
            local first = ...
            local byTable = select("#", ...) == 1 and type(first) == "table" and getmetatable(first) == nil
            for i, f in ipairs(cls.__fields) do
                local v
                if byTable then v = first[f[1]] else v = (select(i, ...)) end
                if v == nil then
                    if f[3] then v = f[2](obj) else v = f[2] end
                end
                rawset(obj, f[1], v)
            end
            return obj
        end,
        __tostring = function() return "struct " .. name end,
    })
    return C
end
function S.sfield(C, name, default, isFn)
    local list = rawget(C, "__fields")
    list[#list + 1] = {name, default, isFn}
end

-- Перечисления: значения — целые с нуля (или заданные), имена — обратно по
-- значению. Обращение к несуществующему значению — ошибка, а не тихий nil:
-- GameState.Plaing иначе превращался бы в «состояние, которого никогда нет».
function S.enum(name)
    return {__name = name, __items = {}, __byValue = {}, __next = 0}
end
function S.item(E, itemName, value)
    if value == nil then value = E.__next end
    if type(value) == "number" then E.__next = value + 1 end
    E[itemName] = value
    E.__items[#E.__items + 1] = itemName
    E.__byValue[value] = itemName
end
function S.seal(E)
    local items = E.__items
    E.name = function(v) return E.__byValue[v] end
    E.values = function()
        local out = {}
        for i, n in ipairs(items) do out[i] = E[n] end
        return out
    end
    E.count = function() return #items end
    E.has = function(v) return E.__byValue[v] ~= nil end
    setmetatable(E, {
        __index = function(_, k) error("в перечислении " .. E.__name .. " нет значения '" .. tostring(k) .. "'", 2) end,
        __newindex = function() error("перечисление " .. E.__name .. " не изменяется", 2) end,
        __tostring = function() return "enum " .. E.__name end,
    })
    return E
end

-- @server / @client: вне своей стороны вызов ничего не делает (и говорит об
-- этом один раз). Одиночная игра — обе стороны сразу.
function S.side(fn, side, name)
    local warned = false
    return function(...)
        if S.role == "both" or S.role == side then return fn(...) end
        if not warned then
            warned = true
            log("Warning", name .. " объявлен @" .. side .. " и на стороне '" .. S.role .. "' не выполняется")
        end
    end
end
function S.setRole(role) S.role = role end

-- ===========================================================================
--  Компоненты
-- ===========================================================================
local hooks = {
    Update = "onUpdate", FixedUpdate = "onFixedUpdate", LateUpdate = "onLateUpdate",
    OnEnable = "onEnable", OnDisable = "onDisable",
    OnCollisionEnter = "onCollisionEnter", OnCollisionExit = "onCollisionExit",
    OnTriggerEnter = "onTriggerEnter", OnTriggerExit = "onTriggerExit", OnTriggerStay = "onTriggerStay",
    OnKeyDown = "onKeyDown", OnKeyUp = "onKeyUp", OnAnimationEvent = "onAnimationEvent",
}
S.hooks = hooks

local Component = S.class("Component", Object, "SAGE")
S.Component = Component
for _, h in pairs(hooks) do Component[h] = function() end end
Component.onAwake = function() end
Component.onStart = function() end
Component.onDestroy = function() end
function Component:getComponent(T) return S.getComponent(self.gameObject, T) end
function Component:addComponent(T, opts) return S.addComponent(self.gameObject, T, opts) end
function Component:hasComponent(T) return S.getComponent(self.gameObject, T) ~= nil end
function Component:destroy() return self.gameObject:Destroy() end

-- Определён ли хук где-то ниже Component (иначе движок его не зовёт вовсе —
-- пустой Update у сотни объектов это сотня вызовов в кадр впустую).
local function defines(C, name)
    local c = C
    while c and c ~= Component and c ~= Object do
        if rawget(c, name) then return true end
        c = rawget(c, "__parent")
    end
    return false
end

S.virtuals = {}   -- объект -> список вспомогательных компонентов
local function virtualsOf(go)
    if go == nil then return nil end
    local ok, id = pcall(function() return go.id end)
    if not ok or id == nil then return nil end
    return S.virtuals[id], id
end

-- Подготовка экземпляра к первому вызову: поля, ссылки, требования.
function S.attach(self, C)
    if rawget(self, "__attached") then return end
    rawset(self, "__attached", true)
    rawset(self, "__class", C)
    local go = rawget(self, "gameObject")
    rawset(self, "entity", go)
    runInits(self, C)
    local meta = rawget(C, "__meta") or {}
    if meta.require and go then
        for _, name in ipairs(meta.require) do
            local okHas, has = pcall(function() return go:HasComponent(name) end)
            if okHas and not has then
                local okAdd, err = pcall(function() return go:AddComponent(name) end)
                if okAdd then log("Warning", C.__name .. ": @require(" .. name .. ") — компонент добавлен к '" .. tostring(go.name) .. "'")
                else log("Error", C.__name .. ": @require(" .. name .. ") — " .. tostring(err)) end
            end
        end
    end
end

local function relay(self, name, ...)
    local list = virtualsOf(rawget(self, "gameObject"))
    if not list then return end
    for _, v in ipairs(list) do
        local fn = v[name]
        if fn then call(v.__class.__name .. ":" .. name, v, fn, v, ...) end
    end
end

function S.component(C)
    rawset(C, "__component", true)
    for engineName, sageName in pairs(hooks) do
        if defines(C, sageName) then
            C[engineName] = function(self, ...)
                local prev = S.current
                S.current = self
                local r = self[sageName](self, ...)
                S.current = prev
                if engineName ~= "Update" and engineName ~= "FixedUpdate" and engineName ~= "LateUpdate" then
                    relay(self, sageName, ...)
                end
                return r
            end
        end
    end
    C.Start = function(self)
        S.attach(self, C)
        local prev = S.current
        S.current = self
        self:onAwake()
        self:onStart()
        S.current = prev
    end
    C.OnDestroy = function(self)
        local prev = S.current
        S.current = self
        local ok, err = pcall(self.onDestroy, self)
        S.current = prev
        relay(self, "onDestroy")
        S.release(self)
        if not ok then error(err, 0) end
    end
end

-- Компонент объекта: класс SAGE, компонент движка или имя.
function S.getComponent(go, T)
    if go == nil or T == nil then return nil end
    if type(T) == "string" then return go:GetComponent(T) end
    if rawget(T, "__engine") then
        if T.__engine == "Transform" then return go.transform end
        return go:GetComponent(T.__engine)
    end
    local script = go:GetScript()
    if script ~= nil and S.is(script, T) then return script end
    -- Скрипт ещё не стартовал (Start впереди): класс известен по таблице.
    if script ~= nil and rawget(script, "__class") == nil then
        local mt = getmetatable(script)
        if T.__name and type(script) == "table" then
            local ok, cls = pcall(function() return script.__name end)
            if ok and cls == T.__name then return script end
        end
    end
    local list = virtualsOf(go)
    if list then
        for _, v in ipairs(list) do if S.is(v, T) then return v end end
    end
    return nil
end

function S.addComponent(go, T, opts)
    if type(T) == "string" then return go:AddComponent(T, opts) end
    if rawget(T, "__engine") then
        if T.__engine == "Transform" then return go.transform end
        return go:AddComponent(T.__engine, opts)
    end
    if not rawget(T, "__component") then error("addComponent: " .. tostring(T.__name) .. " — не компонент", 2) end
    -- Первый компонент объекта — настоящий компонент движка (виден в
    -- инспекторе, сохраняется со сценой). Остальные — вспомогательные: движок
    -- держит один скрипт на объект, их ведёт runtime SAGE.
    if go:GetScript() == nil and T.__path and T.__path ~= "" and T.__path ~= "SAGE" then
        local inst = go:AddComponent("Script", {path = T.__path})
        if type(opts) == "table" and type(inst) == "table" then for k, v in pairs(opts) do inst[k] = v end end
        return inst
    end
    local inst = setmetatable({gameObject = go, transform = go.transform}, {__index = T})
    if type(opts) == "table" then for k, v in pairs(opts) do inst[k] = v end end
    S.attach(inst, T)
    local list, id = virtualsOf(go)
    if id == nil then error("addComponent: объект без номера", 2) end
    if not list then list = {}; S.virtuals[id] = list end
    list[#list + 1] = inst
    call(T.__name .. ":onAwake", inst, inst.onAwake, inst)
    call(T.__name .. ":onStart", inst, inst.onStart, inst)
    return inst
end

function S.removeComponent(go, T)
    local list, id = virtualsOf(go)
    if list then
        for i, v in ipairs(list) do
            if S.is(v, T) then
                call(T.__name .. ":onDestroy", v, v.onDestroy, v)
                S.release(v)
                table.remove(list, i)
                return true
            end
        end
    end
    if type(T) == "string" then return go:RemoveComponent(T) end
    if rawget(T, "__engine") then return go:RemoveComponent(T.__engine) end
    return false
end

-- Всё, чем владел экземпляр (подписки, таймеры, задачи), — снимается.
function S.release(owner)
    rawset(owner, "__released", true)
    for _, list in pairs(S.handlers) do
        for i = #list, 1, -1 do if list[i].owner == owner then list[i].dead = true; table.remove(list, i) end end
    end
    for _, t in ipairs(S.timers) do if t.owner == owner then t.cancelled = true end end
    for _, t in ipairs(S.tasks) do if t.owner == owner then t.cancelled = true end end
end

-- Методы объекта сцены в стиле SAGE: entity:addComponent(Movement).
G.__SageObjectMethods = {
    addComponent = function(self, T, opts) return S.addComponent(self, T, opts) end,
    getComponent = function(self, T) return S.getComponent(self, T) end,
    hasComponent = function(self, T) return S.getComponent(self, T) ~= nil end,
    removeComponent = function(self, T) return S.removeComponent(self, T) end,
    destroy = function(self) return self:Destroy() end,
    keepOnLoad = function(self, on) return self:KeepOnLoad(on) end,
    isValid = function(self) return self:IsValid() end,
}

-- ===========================================================================
--  События
-- ===========================================================================
S.handlers = {}
local function eventName(ev)
    if type(ev) == "table" and ev.__event then return ev.__event end
    if type(ev) == "string" then return ev end
    error("Events: ожидалось событие или его имя, получено " .. type(ev), 3)
end
function S.event(name, params)
    return setmetatable({__event = name, name = name, params = params or {}},
                        {__tostring = function() return "event " .. name end})
end

local Events = {}
local function connect(ev, fn, once)
    if type(fn) ~= "function" then error("Events.on: второй аргумент — функция-обработчик", 3) end
    local name = eventName(ev)
    local list = S.handlers[name]
    if not list then list = {}; S.handlers[name] = list end
    local h = {fn = fn, once = once, owner = S.current, name = name}
    h.disconnect = function()
        if h.dead then return false end
        h.dead = true
        local l = S.handlers[name]
        if l then for i = #l, 1, -1 do if l[i] == h then table.remove(l, i) end end end
        return true
    end
    h.cancel = h.disconnect
    list[#list + 1] = h
    return h
end
function Events.on(ev, fn) return connect(ev, fn, false) end
function Events.once(ev, fn) return connect(ev, fn, true) end
function Events.off(what)
    if type(what) == "table" and what.disconnect then return what.disconnect() end
    local name = eventName(what)
    local n = S.handlers[name] and #S.handlers[name] or 0
    S.handlers[name] = nil
    return n > 0
end
function Events.count(ev)
    local list = S.handlers[eventName(ev)]
    return list and #list or 0
end
-- Отправка безопасна посреди обновления сцены: список обработчиков снимается
-- ДО вызовов, и обработчик вправе подписывать и отписывать кого угодно.
function Events.emit(ev, ...)
    local name = eventName(ev)
    local list = S.handlers[name]
    local delivered = 0
    if list and #list > 0 then
        local snapshot = {unpack(list)}
        for _, h in ipairs(snapshot) do
            if not h.dead then
                if not owned(h.owner) then
                    h.disconnect()
                else
                    if h.once then h.disconnect() end
                    call("обработчик события " .. name, h.owner, h.fn, ...)
                    delivered = delivered + 1
                end
            end
        end
    end
    -- И в шину движка: обычные Lua-скрипты и связи инспектора тоже слышат.
    local E = rawget(G, "Events")
    if E and E.emit and E ~= Events then pcall(E.emit, name, (...)) end
    return delivered
end
Events.On, Events.Once, Events.Emit, Events.Off = Events.on, Events.once, Events.emit, Events.off

)LUA";

} // namespace sage::lang

#include "sage/lang/SageCompiler.h"

// ---------------------------------------------------------------------------
// SAGE Lua: RUNTIME.
//
// То, на чём исполняется сгенерированный код: классы и наследование,
// перечисления и структуры, компоненты и их жизненный цикл, события,
// планировщик задач (async/await, таймеры), Result, обёртки API движка в
// стиле SAGE (Input.isDown, Scene.createEntity, Log.info…).
//
// Написан на Lua, а не на C++, потому что почти весь — про таблицы и
// метатаблицы Lua: на C++ то же самое было бы втрое длиннее и всё равно
// работало бы через стек Lua. Обращения к движку идут через уже существующие
// разделы (Input, Scene, Debug, Physics…) — второй привязки к C++ здесь нет.
//
// Ставится ОДИН раз на состояние Lua (глобальная таблица __sage). Файл .slua
// видит обёртки локальными именами (см. шапку генератора), поэтому обычные
// Lua-скрипты рядом продолжают видеть прежние Input/Scene/Time как были.
// ---------------------------------------------------------------------------
namespace sage::lang {

const char* PreludeSource() {
    return R"LUA(
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

-- ===========================================================================
--  Планировщик: задачи (async/await) и таймеры
-- ===========================================================================
S.tasks = {}
S.timers = {}
S.currentTask = nil

local Task = {}
Task.__index = Task
function Task:isDone() return self.done == true end
function Task:cancel() self.cancelled = true end
function Task:result()
    if self.err then error(self.err, 2) end
    return self.value
end

local function finish(task, ok, value)
    task.done = true
    if ok then task.value = value else task.err = value end
end

local function resume(task, ...)
    local prevTask, prevOwner = S.currentTask, S.current
    S.currentTask, S.current = task, task.owner
    local ok, req = coroutine.resume(task.co, ...)
    S.currentTask, S.current = prevTask, prevOwner
    if not ok then
        finish(task, false, req)
        report("async-задача", req, task.co)
        return
    end
    if coroutine.status(task.co) == "dead" then
        finish(task, true, req)
        return
    end
    task.wait = req
    if req and req.seconds then req.left = req.seconds end
    if not task.queued then
        task.queued = true
        S.tasks[#S.tasks + 1] = task
    end
end

function S.run(fn, ...)
    local task = setmetatable({__task = true, co = coroutine.create(fn), owner = S.current}, Task)
    resume(task, ...)
    return task
end

function S.await(x)
    if S.currentTask == nil or coroutine.running() ~= S.currentTask.co then
        error("await вне async-функции", 2)
    end
    if type(x) ~= "table" then return x end
    if x.__task then
        if not x.done then coroutine.yield({task = x}) end
        if x.err then error(x.err, 0) end
        return x.value
    end
    if x.__wait then return coroutine.yield(x) end
    return x
end

local function ready(task, dt)
    local w = task.wait
    if w == nil then return true end
    if w.task then return w.task.done end
    if w.seconds then
        w.left = w.left - dt
        return w.left <= 0
    end
    if w.frames then
        w.frames = w.frames - 1
        return w.frames <= 0
    end
    if w.cond then
        local ok, r = pcall(w.cond)
        return not ok or r
    end
    return true
end

function S.tickTasks(dt)
    if #S.tasks == 0 then return end
    local list = S.tasks
    S.tasks = {}
    for _, task in ipairs(list) do
        if task.cancelled or not owned(task.owner) then
            task.done = true
            task.cancelled = true
        elseif ready(task, dt) then
            task.queued = false
            task.wait = nil
            resume(task)
        else
            S.tasks[#S.tasks + 1] = task
        end
    end
end

local Timer = {}
local function addTimer(delay, fn, interval, count)
    if type(fn) ~= "function" then error("Timer: второй аргумент — функция", 3) end
    local t = {left = delay, fn = fn, interval = interval, count = count, owner = S.current}
    t.cancel = function() t.cancelled = true end
    t.isActive = function() return not t.cancelled and not t.finished end
    S.timers[#S.timers + 1] = t
    return t
end
function Timer.after(seconds, fn) return addTimer(seconds, fn, nil, 1) end
function Timer.every(seconds, fn, count)
    if seconds <= 0 then error("Timer.every: интервал должен быть больше нуля", 2) end
    return addTimer(seconds, fn, seconds, count)
end
function Timer.cancelAll() for _, t in ipairs(S.timers) do t.cancelled = true end end

function S.tickTimers(dt)
    if #S.timers == 0 then return end
    local list = S.timers
    S.timers = {}
    local keep = {}
    for _, t in ipairs(list) do
        if not t.cancelled and owned(t.owner) then
            t.left = t.left - dt
            local fired = 0
            while not t.cancelled and t.left <= 0 do
                call("таймер", t.owner, t.fn)
                fired = fired + 1
                if t.count then t.count = t.count - 1 end
                if t.interval and (t.count == nil or t.count > 0) and fired < 1000 then
                    t.left = t.left + t.interval
                else
                    t.finished = true
                    break
                end
            end
            if not t.finished and not t.cancelled then keep[#keep + 1] = t end
        end
    end
    for _, t in ipairs(S.timers) do keep[#keep + 1] = t end   -- заведённые внутри
    S.timers = keep
end

-- Кадр runtime: вспомогательные компоненты, таймеры, задачи.
function S.tick(dt)
    for id, list in pairs(S.virtuals) do
        local alive = {}
        for _, v in ipairs(list) do
            if not owned(v) then
                call(v.__class.__name .. ":onDestroy", v, v.onDestroy, v)
                S.release(v)
            else
                local up = v.onUpdate
                if up and up ~= Component.onUpdate then call(v.__class.__name .. ":onUpdate", v, up, v, dt) end
                local late = v.onLateUpdate
                if late and late ~= Component.onLateUpdate then call(v.__class.__name .. ":onLateUpdate", v, late, v, dt) end
                alive[#alive + 1] = v
            end
        end
        if #alive == 0 then S.virtuals[id] = nil else S.virtuals[id] = alive end
    end
    S.tickTimers(dt)
    S.tickTasks(dt)
end

function S.reset()
    S.tasks, S.timers, S.handlers, S.virtuals = {}, {}, {}, {}
    S.loaded, S.loading = {}, {}
    S.current, S.currentTask = nil, nil
end

-- ===========================================================================
--  Result<T, E>
-- ===========================================================================
local Result = {__name = "Result"}
Result.__index = Result
Result.__tostring = function(r)
    if r._ok then return "Ok(" .. tostring(r._v) .. ")" end
    return "Err(" .. tostring(r._e) .. ")"
end
function Result.ok(v) return setmetatable({_ok = true, _v = v}, Result) end
function Result.err(e) return setmetatable({_ok = false, _e = e}, Result) end
function Result:isOk() return self._ok end
function Result:isErr() return not self._ok end
function Result:value()
    if not self._ok then error("Result: value() у ошибки: " .. tostring(self._e), 2) end
    return self._v
end
Result.unwrap = Result.value
function Result:error() return self._e end
function Result:unwrapOr(fallback) if self._ok then return self._v end return fallback end
function Result:map(fn) if self._ok then return Result.ok(fn(self._v)) end return self end

-- ===========================================================================
--  Обёртки API движка
-- ===========================================================================
local function proxy(globalName, own)
    return setmetatable(own, {__index = function(_, k)
        local g = rawget(G, globalName)
        if g ~= nil then return g[k] end
        return nil
    end})
end

local Log = {
    info = function(...) log("Log", ...) end,
    warn = function(...) log("Warning", ...) end,
    error = function(...) log("Error", ...) end,
    debug = function(...) log("Log", ...) end,
}

local function gin() return rawget(G, "Input") end
local function has(action)
    local I = gin()
    return I and I.HasAction and I.HasAction(action)
end
local Input = proxy("Input", {})
local padActions = {}
local function padAction(button)
    local name = "__pad_" .. button
    if not padActions[name] then
        padActions[name] = true
        gin().BindAction(name, "PAD_" .. string.upper(button))
    end
    return name
end
function Input.isDown(a)
    local I = gin()
    if has(a) then return I.IsActionDown(a) end
    return I.IsKeyDown(a) or I.IsMouseButtonDown(a)
end
function Input.isPressed(a)
    local I = gin()
    if has(a) then return I.IsActionPressed(a) end
    return I.IsKeyPressed(a) or I.IsMouseButtonPressed(a)
end
function Input.isReleased(a)
    local I = gin()
    if has(a) then return I.IsActionReleased(a) end
    return false
end
local function keyAxis(I, pos, neg)
    local v = 0
    for _, k in ipairs(pos) do if I.IsKeyDown(k) then v = v + 1; break end end
    for _, k in ipairs(neg) do if I.IsKeyDown(k) then v = v - 1; break end end
    return v
end
function Input.getAxis(a)
    local I = gin()
    if has(a) then return I.GetAxis(a) end
    if a == "Horizontal" then return keyAxis(I, {"D", "Right"}, {"A", "Left"}) end
    if a == "Vertical" then return keyAxis(I, {"W", "Up"}, {"S", "Down"}) end
    return 0
end
function Input.getAxis2D(a)
    local I = gin()
    if has(a) then return I.GetVector2(a) end
    local V2 = rawget(G, "Vector2")
    if a == "Move" then
        return V2(keyAxis(I, {"D", "Right"}, {"A", "Left"}), keyAxis(I, {"W", "Up"}, {"S", "Down"}))
    end
    return V2(0, 0)
end
function Input.bind(action, src)
    local I = gin()
    if type(src) == "string" then return I.BindAction(action, src) end
    if type(src) == "table" then
        if src.positive or src.negative then return I.BindAxis(action, src.positive or "", src.negative or "") end
        if src.up or src.down or src.left or src.right then
            return I.BindVector(action, src.up or "", src.down or "", src.left or "", src.right or "")
        end
        local ok = false
        for _, s in ipairs(src) do ok = I.BindAction(action, s) or ok end
        return ok
    end
    error("Input.bind: источник — клавиша (Key.A), кнопка мыши или таблица {negative=…, positive=…}", 2)
end
function Input.getMousePosition() return gin().GetMousePosition() end
function Input.getMouseDelta() return gin().GetMouseDelta() end
function Input.getMouseWheel() return gin().GetMouseWheel() end
function Input.setCursorLocked(on) return gin().SetCursorCaptured(on ~= false) end
function Input.isCursorLocked() return gin().IsCursorCaptured() end
function Input.getGamepad(index)
    return {
        index = index or 0,
        isDown = function(_, b) return gin().IsActionDown(padAction(b)) end,
        isPressed = function(_, b) return gin().IsActionPressed(padAction(b)) end,
    }
end

local Key = setmetatable({}, {__index = function(_, k) return k end})
local Mouse = {Left = "MOUSE_LEFT", Right = "MOUSE_RIGHT", Middle = "MOUSE_MIDDLE"}

local Time = proxy("Time", {})
function Time.wait(seconds) return {__wait = true, seconds = seconds or 0} end
function Time.nextFrame() return {__wait = true, frames = 1} end
function Time.waitFrames(n) return {__wait = true, frames = n or 1} end
function Time.waitUntil(fn) return {__wait = true, cond = fn} end

local function gscene() return rawget(G, "Scene") end
local Scene = proxy("Scene", {})
function Scene.createEntity(name) return gscene().Create(name) end
function Scene.instantiate(prefab, position)
    local path = type(prefab) == "table" and prefab.path or prefab
    return gscene().Instantiate(path, position)
end
function Scene.findEntity(name) return gscene().Find(name) end
function Scene.findById(id) return gscene().FindById(id) end
function Scene.findWithTag(tag) return gscene().FindByTag(tag) end
function Scene.findAllWithTag(tag) return gscene().FindAllByTag(tag) end
function Scene.destroy(e) if e ~= nil then return e:Destroy() end end
function Scene.load(name, params)
    local n = type(name) == "table" and name.path or name
    gscene().Load(n, params)
    return {path = n, __scene = true}
end
function Scene.reload() return gscene().Reload() end
-- Параметры, с которыми пришли в эту сцену, и откуда пришли.
function Scene.params() return gscene().Params() end
function Scene.previous() return gscene().Previous() end
function Scene.exists(name) return gscene().Exists(name) end
function Scene.list() return gscene().List() end
-- Сцена поверх сцены: возвращает корни добавленного; убирается группой.
function Scene.loadAdditive(name, offset, group) return gscene().LoadAdditive(name, offset, group) end
function Scene.unloadAdditive(group) return gscene().UnloadAdditive(group) end
function Scene.tryLoad(name)
    if name == nil or name == "" then return nil end
    return {path = name, __scene = true}
end
function Scene.setActive(s)
    if type(s) == "table" and s.path then return gscene().Load(s.path) end
end
function Scene.getActive() return gscene().Name() end

local Assets = {}
local native = rawget(G, "__sage_native_exists")
function Assets.exists(path) return native == nil or native(path) end
-- Загрузка — задача: await Assets.load<Texture>("Textures/Hero").
function Assets.load(path, kind)
    return S.run(function()
        if not Assets.exists(path) then error("ресурс не найден: " .. tostring(path), 0) end
        return {path = path, kind = kind, __asset = true}
    end)
end

local Physics = proxy("Physics", {})
function Physics.raycast(origin, direction, distance, mask)
    local hit = rawget(G, "Physics").Raycast(origin, direction, distance or 1000, mask)
    if hit then hit.collider = hit.object; hit.entity = hit.object end
    return hit
end
function Physics.overlapSphere(center, radius, mask) return rawget(G, "Physics").OverlapSphere(center, radius, mask) end

local Mathf = {
    pi = math.pi, deg2rad = math.pi / 180, rad2deg = 180 / math.pi, epsilon = 1e-6, infinity = math.huge,
    abs = math.abs, min = math.min, max = math.max, floor = math.floor, ceil = math.ceil, sqrt = math.sqrt,
    sin = math.sin, cos = math.cos, tan = math.tan, atan2 = function(y, x) return math.atan(y, x) end,
}
function Mathf.clamp(v, lo, hi) if v < lo then return lo elseif v > hi then return hi end return v end
function Mathf.clamp01(v) return Mathf.clamp(v, 0, 1) end
function Mathf.lerp(a, b, t) return a + (b - a) * Mathf.clamp01(t) end
function Mathf.inverseLerp(a, b, v) if a == b then return 0 end return Mathf.clamp01((v - a) / (b - a)) end
function Mathf.sign(v) if v > 0 then return 1 elseif v < 0 then return -1 end return 0 end
function Mathf.round(v) return math.floor(v + 0.5) end
function Mathf.moveTowards(cur, target, delta)
    if math.abs(target - cur) <= delta then return target end
    return cur + Mathf.sign(target - cur) * delta
end
function Mathf.approximately(a, b) return math.abs(a - b) < 1e-5 end

-- Vector3.zero и прочие постоянные — СВОЙСТВА, но каждый раз новый вектор:
-- общий объект правился бы на месте у всех скриптов сразу.
local function vectorProxy(globalName, consts)
    return setmetatable({}, {
        __index = function(_, k)
            local mk = consts[k]
            if mk then return mk() end
            local g = rawget(G, globalName)
            return g and g[k]
        end,
        __call = function(_, ...) return rawget(G, globalName)(...) end,
    })
end
local function V3(x, y, z) return rawget(G, "Vector3")(x, y, z) end
local Vector3 = vectorProxy("Vector3", {
    zero = function() return V3(0, 0, 0) end, one = function() return V3(1, 1, 1) end,
    up = function() return V3(0, 1, 0) end, down = function() return V3(0, -1, 0) end,
    right = function() return V3(1, 0, 0) end, left = function() return V3(-1, 0, 0) end,
    forward = function() return V3(0, 0, -1) end, back = function() return V3(0, 0, 1) end,
})
local function V2(x, y) return rawget(G, "Vector2")(x, y) end
local Vector2 = vectorProxy("Vector2", {
    zero = function() return V2(0, 0) end, one = function() return V2(1, 1) end,
    up = function() return V2(0, 1) end, right = function() return V2(1, 0) end,
})
local Quaternion = vectorProxy("Quaternion", {
    identity = function() return rawget(G, "Quaternion").Identity() end,
})

-- Компоненты движка как значения: getComponent(AudioSource).
local function engineComponent(name, engine)
    return setmetatable({__name = name, __engine = engine}, {__tostring = function() return name end})
end

local Entity = S.class("Entity", Object, "SAGE")

S.api = {
    Events = Events, Input = Input, Time = Time, Scene = Scene, Log = Log, Timer = Timer,
    Assets = Assets, Physics = Physics, Result = Result, Key = Key, Mouse = Mouse, Mathf = Mathf,
    Component = Component, Entity = Entity, Vector2 = Vector2, Vector3 = Vector3, Quaternion = Quaternion,
    Transform = engineComponent("Transform", "Transform"), Camera = engineComponent("Camera", "Camera"),
    RigidBody = engineComponent("RigidBody", "RigidBody"), Collider = engineComponent("Collider", "Collider"),
    AudioSource = engineComponent("AudioSource", "Audio"), Animator = engineComponent("Animator", "Animation"),
    ParticleEmitter = engineComponent("ParticleEmitter", "Particles"),
    CharacterController = engineComponent("CharacterController", "CharacterController"),
    Light = engineComponent("Light", "Light"), Mesh = engineComponent("Mesh", "Mesh"),
}
-- import { X } from "SAGE/Core": обёртки SAGE, затем глобальные разделы движка.
S.core = setmetatable({}, {__index = function(_, k)
    local v = S.api[k]
    if v ~= nil then return v end
    return rawget(G, k)
end})

-- ===========================================================================
--  Модули
-- ===========================================================================
function S.module(path, exports) S.modules[path] = exports end

function S.import(spec, from)
    if string.sub(spec, 1, 4) == "SAGE" then return S.core end
    local load_ = rawget(G, "__sage_native_load")
    if load_ == nil then error("import: загрузчик модулей не установлен", 2) end
    local code, path = load_(spec, from or "")
    if code == nil then error(path or ("модуль '" .. spec .. "' не найден"), 2) end
    local cached = S.loaded[path]
    if cached ~= nil then return cached end
    if S.loading[path] then return S.modules[path] or {} end
    S.loading[path] = true
    local env = setmetatable({}, {__index = G})
    local fn, err = load(code, "@" .. path, "t", env)
    if not fn then
        S.loading[path] = nil
        error(err, 2)
    end
    local ok, result = pcall(fn)
    S.loading[path] = nil
    if not ok then error(result, 0) end
    local exports = S.modules[path]
    if exports == nil or (next(exports) == nil and type(result) == "table") then exports = result end
    if exports == nil then exports = {} end
    S.loaded[path] = exports
    return exports
end

return S
)LUA";
}

} // namespace sage::lang

#pragma once

// Части прелюдии SAGE Lua (см. SagePrelude.cpp: PreludeSource() склеивает их
// при компиляции, байт в байт). Разделена по смыслу: модель объектов языка
// отдельно от того, что работает в кадре.
namespace sage::lang {

// Планировщик задач (async/await, таймеры), Result, обёртки API движка, импорт.
inline constexpr char kPreludeRuntime[] = R"LUA(-- ===========================================================================
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

} // namespace sage::lang

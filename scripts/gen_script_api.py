#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Подсказка по скриптовому API для ЛЮБОГО редактора кода.

ЗАЧЕМ. Скрипты пишут не в SAGE — своего редактора кода у движка нет намеренно
(см. editor/src/CodeEditorApp.h). Пишут их в VS Code, Neovim, JetBrains, Zed. И
до сих пор там не было ни одной подсказки: движок даёт под три сотни функций в
двух с половиной десятках модулей, а редактор про них не знает ничего — ни
имени, ни аргументов. Единственным способом узнать, как называется функция и
что она берёт, было чтение исходников движка на C++.

ЧТО ЭТО ЗА ФАЙЛ. Описание API на языке аннотаций LuaLS/EmmyLua — его понимают
ВСЕ распространённые редакторы, потому что за подсказками к Lua в каждом из них
стоит один и тот же lua-language-server (VS Code «Lua» от sumneko, Neovim,
Zed, Helix) либо совместимый с ним EmmyLua (JetBrains). Это не «файл для VS
Code», а общий формат — ровно поэтому его и выбрали.

ПОЧЕМУ ГЕНЕРАТОР, А НЕ ФАЙЛ РУКАМИ. Подсказка, написанная руками, расходится с
движком на первой же правке — и начинает ВРАТЬ: предлагает функцию, которой
нет, и молчит о той, что появилась. Врущая подсказка хуже отсутствующей:
отсутствующую человек компенсирует документацией, а врущей он верит. Поэтому
источник правды один — вызовы Bind(...) в engine/src/sage/scripting/, то есть
ровно то, что движок регистрирует на самом деле.

Свежесть файла сторожит scripts/gen_script_api.py --check: разошёлся — CI падает.

Запуск:
    python3 scripts/gen_script_api.py            # переписать editor/assets/api/sage.lua
    python3 scripts/gen_script_api.py --check    # только сверить, ничего не писать
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = sorted((ROOT / "engine/src/sage/scripting").glob("*.cpp"))
OUT = ROOT / "editor/assets/api/sage.lua"

# Bind("модуль", "Имя", "СтароеИмя"|nullptr, [захват](аргументы) -> тип {
BIND = re.compile(
    r'\bBind\(\s*"([a-z_]+)"\s*,\s*"(\w+)"\s*,\s*(nullptr|"[\w]+")\s*,\s*'
    r'\[[^\]]*\]\s*\(([^)]*)\)\s*(?:->\s*([^{]+?)\s*)?\{',
    re.S)

# C++ -> Lua. Неизвестное осознанно становится any: соврать типом хуже, чем
# промолчать о нём — подсказка с неверным типом уводит в сторону молча.
TYPES = [
    (r'\bglm::vec2\b', 'Vec2'), (r'\bglm::vec3\b', 'Vec3'), (r'\bglm::vec4\b', 'Vec4'),
    (r'\bglm::quat\b', 'Vec4'),
    (r'\bGameObject\b', 'Entity'),
    (r'\bstd::string\b', 'string'),
    (r'\bbool\b', 'boolean'),
    (r'\b(?:float|double|int|unsigned|size_t|int64_t|uint32_t|long long)\b', 'number'),
    (r'\bsol::table\b', 'table'),
    (r'\bsol::(?:protected_function|function)\b', 'function'),
    (r'\bsol::(?:object|variadic_args|nil_t)\b', 'any'),
    (r'\bvoid\b', 'nil'),
]


def lua_type(cpp):
    """Тип аргумента или возврата. Второе значение — необязательный ли он."""
    text = cpp.strip()
    optional = False
    m = re.search(r'sol::optional\s*<\s*(.+?)\s*>\s*$', text)
    if m:
        optional = True
        text = m.group(1)
    text = re.sub(r'\b(const|&|\*)\b', ' ', text).replace('&', ' ').replace('*', ' ')
    for pattern, name in TYPES:
        if re.search(pattern, text):
            return name, optional
    return 'any', optional


def split_args(text):
    """Аргументы лямбды: по запятым нулевого уровня (в типах есть свои)."""
    out, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        elif ch == ',' and depth == 0:
            out.append(text[start:i])
            start = i + 1
    out.append(text[start:])
    return [a for a in (x.strip() for x in out) if a]


def arg_name(cpp, index):
    """Имя аргумента из объявления; безымянному даём номер."""
    m = re.search(r'(\w+)\s*$', cpp.strip())
    name = m.group(1) if m else ''
    if not name or name in ('float', 'int', 'bool', 'double', 'unsigned', 'string',
                            'GameObject', 'table', 'object', 'size_t'):
        return f'arg{index + 1}'
    # `self` и `obj` у методов сущности читаются понятнее как entity
    return 'entity' if name in ('obj', 'object') else name


def collect():
    """{модуль: [(имя, [(имя_арг, тип, необязателен)], тип_возврата, старое_имя)]}"""
    modules = {}
    legacy = []
    for path in SOURCES:
        text = path.read_text(encoding='utf-8')
        for m in BIND.finditer(text):
            module, name, old, args, ret = m.groups()
            params = []
            for i, a in enumerate(split_args(args)):
                t, opt = lua_type(a)
                params.append((arg_name(a, i), t, opt))
            rtype = lua_type(ret)[0] if ret else 'nil'
            modules.setdefault(module, []).append((name, params, rtype))
            if old != 'nullptr':
                legacy.append((old.strip('"'), module, name))
    for fns in modules.values():
        fns.sort(key=lambda f: f[0])
    return modules, sorted(legacy)


HEADER = '''---@meta
-- ===========================================================================
--  ПОДСКАЗКА ПО СКРИПТОВОМУ API SAGE — для любого редактора кода.
--
--  ФАЙЛ СОБРАН АВТОМАТИЧЕСКИ из вызовов Bind(...) в engine/src/sage/scripting/.
--  Править руками бессмысленно: следующий запуск scripts/gen_script_api.py
--  перезапишет. Нужна другая подсказка — меняйте движок, она поедет следом.
--
--  КАК ПОДКЛЮЧИТЬ. В новом проекте уже подключено: рядом с ним лежат
--  .luarc.json и .sage/api/sage.lua, и редактор находит их сам. Чужому проекту
--  хватит .luarc.json со строкой
--      { "workspace.library": [".sage/api"] }
--
--  ДВА ИМЕНИ У КАЖДОЙ ФУНКЦИИ, и оба настоящие:
--      sage.game.Quit()   -- полное: видно, чьё оно, и его не затрёт своя функция
--      game.Quit()        -- короткое: тот же самый объект, а не копия
--  Короткое имя — это ПСЕВДОНИМ модуля в глобальных, и движок ставит его,
--  только если имя свободно: игра, объявившая свою `scene`, остаётся при своей.
-- ===========================================================================

---@class Vec2
---@field x number
---@field y number
Vec2 = {}
---@param x number
---@param y number
---@return Vec2
function Vec2.new(x, y) end

---@class Vec3
---@field x number
---@field y number
---@field z number
Vec3 = {}
---@param x number
---@param y number
---@param z number
---@return Vec3
function Vec3.new(x, y, z) end

---@class Vec4
---@field x number
---@field y number
---@field z number
---@field w number
Vec4 = {}
---@param x number
---@param y number
---@param z number
---@param w number
---@return Vec4
function Vec4.new(x, y, z, w) end

---@class Transform
---@field Position Vec3
---@field Rotation Vec3
---@field Scale Vec3

---Сущность сцены: то, что приходит в OnStart/OnUpdate первым аргументом.
---@class Entity
---@field Name string
---@field Transform Transform
---@field Color Vec3

-- ---------------------------------------------------------------------------
--  ПУБЛИЧНЫЕ ПЕРЕМЕННЫЕ СКРИПТА — настройка, своя у КАЖДОГО объекта (две двери
--  с разной скоростью — один скрипт, а не два), видна и правится в инспекторе,
--  лежит в сцене. Объявляются таблицей Vars В НАЧАЛЕ ФАЙЛА, ДО любого кода —
--  движок читает её РАЗБОРОМ ТЕКСТА, а не запуском скрипта, поэтому объявление
--  обязано быть видно глазами, а не собираться функцией:
--
--      Vars = {
--          speed  = 3.0,                                  -- число, умолчание 3.0
--          damage = { 10, min = 0, max = 100, label = "Урон" },  -- число с границами
--          target = { kind = "entity", label = "Цель" },  -- ссылка на объект
--          sound  = { kind = "asset" },                   -- ссылка на файл проекта
--      }
--
--      function OnUpdate(entity, dt)
--          entity.Transform.Position.y = entity.Transform.Position.y
--              + entity:Vars().speed * dt      -- или self:Vars().speed
--      end
--
--  Виды (kind): bool, int, float, string, vec2, vec3, color, entity, asset —
--  без kind число берётся тем, что записано (3 -> int, 3.0 -> float).
--  Подробнее — docs/scripting.md, раздел «Публичные переменные, ссылки и
--  события».
-- ---------------------------------------------------------------------------

---Публичные переменные ЭТОГО объекта — значения из инспектора/сцены по
---именам, объявленным в Vars (см. выше). `entity:Vars().speed`, не
---`Vars.speed`: второе — только объявление вида, без значения объекта.
---@param self Entity
---@return table
function Entity:Vars() end

---Имена всех публичных переменных этого объекта — для инструментов и отладки.
---@param self Entity
---@return string[]
function Entity:VarNames() end

---Есть ли у объекта переменная с таким именем. Отдельно от чтения: значение
---nil законно, и отличить «нет переменной» от «переменная равна nil» иначе
---нечем.
---@param self Entity
---@param name string
---@return boolean
function Entity:HasVar(name) end

---СВОЯ сущность — та, к которой привязан этот скрипт. Видна во всём файле:
---и в коде верхнего уровня, и в функциях, вынесенных из хуков. Внутри хука это
---тот же объект, что и его аргумент: `self == entity`.
---@type Entity
self = nil

---Пишет строку в консоль редактора и в лог игры.
---@param message any
function log(message) end

---То же самое, что log(), но настоящим Lua print: любое число аргументов,
---через таб, tostring на каждый. Стандартный print пишет в stdout, которого
---у игры нет (ни в редакторе, ни в сборке) — вывод улетал бы в никуда молча,
---поэтому здесь print ПЕРЕОПРЕДЕЛЁН на тот же путь, что у log()/logWarn()/
---logError() (консоль редактора + файл лога игры).
---@param ... any
function print(...) end

---Ждать внутри StartCoroutine. Вне корутины смысла не имеет.
---@param seconds number
function wait(seconds) end

-- ===========================================================================
--  НОВАЯ СИСТЕМА СКРИПТИНГА: скрипт-таблица, публичные поля, разделы API.
--
--  Этот раздел написан РУКАМИ, а не собран из Bind(...), и это не оплошность:
--  новый API — не набор функций в модулях, а типы с методами
--  (transform:Translate, character:Move, animator:Play). Собрать такое
--  разбором вызовов нельзя, а без подсказки к нему редактор кода молчит о
--  главном, чем теперь пишут игру.
--
--  Скрипт выглядит так:
--
--      local Player = {}
--
--      Player.public = {
--          MoveSpeed = field.number(5.0, 0.0, 20.0),
--          Camera    = field.entity(),
--      }
--
--      function Player:Start() end
--      function Player:Update(dt) end
--
--      return Player
--
--  Подробнее — docs/scripting.md.
-- ===========================================================================

---Объявление публичного поля. Читается РАЗБОРОМ ТЕКСТА, поэтому пишется прямо
---в таблице `public`, а не собирается функцией.
field = {}
---@param default? number
---@param min? number
---@param max? number
---@return any
function field.number(default, min, max) end
---@param default? integer
---@param min? number
---@param max? number
---@return any
function field.integer(default, min, max) end
---@param default? boolean
---@return any
function field.boolean(default) end
---@param default? string
---@return any
function field.string(default) end
---Ссылка на объект сцены: слот с приёмом перетаскивания в инспекторе.
---@return any
function field.entity() end
---Ссылка на КОМПОНЕНТ объекта: скрипт получает сразу компонент, а не объект.
---@param component string
---@return any
function field.component(component) end
---@return any
function field.color() end
---@return any
function field.vector2() end
---@return any
function field.vector3() end
---Ссылка на файл проекта. kind: "Texture", "Audio", "Animation", "Prefab".
---@param kind? string
---@return any
function field.asset(kind) end

---@class SageTransform
---@field position Vec3
---@field rotation Vec3 углы Эйлера, ГРАДУСЫ
---@field scale Vec3
---@field localPosition Vec3
---@field localRotation Vec3
---@field localScale Vec3
local SageTransform = {}
function SageTransform:SetPosition(x, y, z) end
function SageTransform:SetRotation(x, y, z) end
function SageTransform:SetLocalRotation(x, y, z) end
function SageTransform:SetScale(x, y, z) end
---@param delta Vec3
function SageTransform:Translate(delta) end
function SageTransform:Rotate(x, y, z) end
---@return Vec3
function SageTransform:Forward() end
---@return Vec3
function SageTransform:Right() end
---@return Vec3
function SageTransform:Up() end
---@param target Vec3
function SageTransform:LookAt(target) end
---Позиция с учётом цепочки родителей.
---@return Vec3
function SageTransform:WorldPosition() end

---Объект сцены.
---@class SageObject
---@field name string
---@field id integer
---@field active boolean включён (виден и участвует в кадре)
---@field transform SageTransform
---@field parent SageObject|nil
---@field children SageObject[]
local SageObject = {}
---@param name string
---@return any компонент или nil
function SageObject:GetComponent(name) end
---@return SageCharacterController|nil
function SageObject:GetCharacterController() end
---@return SageAnimation|nil
function SageObject:GetAnimation() end
---@return SageAudioSource|nil
function SageObject:GetAudio() end
---@return SageCamera|nil
function SageObject:GetCamera() end
---Таблица скрипта этого объекта (или nil, если скрипта нет).
---@return table|nil
function SageObject:GetScript() end
---Позвать метод скрипта этого объекта: `enemy:Call("TakeDamage", 20)`.
---@param method string
---@return any
function SageObject:Call(method, ...) end
---@param parent SageObject|nil
function SageObject:SetParent(parent) end
function SageObject:Destroy() end
---@return boolean
function SageObject:IsValid() end

---Контроллер персонажа: универсальное управляемое тело. Тяготение, опора,
---склон и ступенька — внутри него, а не в скрипте.
---@class SageCharacterController
local SageCharacterController = {}
---Горизонтальная СКОРОСТЬ (ед/с) на этот кадр.
---@param velocity Vec3
function SageCharacterController:Move(velocity) end
---Полная скорость, вертикаль включительно (полёт, плавание, отдача).
---@param velocity Vec3
function SageCharacterController:MoveVelocity(velocity) end
---Прыжок заданной ВЫСОТЫ (её видно в игре, в отличие от скорости).
---@param height number
function SageCharacterController:Jump(height) end
---@param velocity number
function SageCharacterController:SetJumpVelocity(velocity) end
---@param gravity number
function SageCharacterController:SetGravity(gravity) end
---@return number
function SageCharacterController:GetGravity() end
---@return boolean
function SageCharacterController:IsGrounded() end
---Действительная скорость последнего шага (стена и склон видны именно здесь).
---@return Vec3
function SageCharacterController:GetVelocity() end
---@return Vec3
function SageCharacterController:GetGroundNormal() end
---@return boolean
function SageCharacterController:Landed() end
---@return boolean
function SageCharacterController:LeftGround() end
---@return boolean
function SageCharacterController:IsBlocked() end
---@param position Vec3
function SageCharacterController:Teleport(position) end

---@class SageAnimation
local SageAnimation = {}
---@param clip string
---@param loop? boolean
---@return boolean
function SageAnimation:Play(clip, loop) end
function SageAnimation:Stop() end
function SageAnimation:Resume() end
---@param speed number
function SageAnimation:SetSpeed(speed) end
---@return string
function SageAnimation:GetCurrentAnimation() end
---@param clip? string
---@return boolean
function SageAnimation:IsPlaying(clip) end
---@param name string
---@param value number
function SageAnimation:SetFloat(name, value) end
---@param name string
---@return number
function SageAnimation:GetFloat(name) end
---@param name string
---@param value boolean
function SageAnimation:SetBool(name, value) end
---@param name string
---@return boolean
function SageAnimation:GetBool(name) end
---@param name string
function SageAnimation:SetTrigger(name) end
---Прочитал — потребил.
---@param name string
---@return boolean
function SageAnimation:ConsumeTrigger(name) end
---@param name string
---@param value number
function SageAnimation:SetBlend(name, value) end
---@param layer string
---@param weight number
function SageAnimation:SetLayerWeight(layer, weight) end
---@param layer string
---@return number
function SageAnimation:GetLayerWeight(layer) end

---@class SageAudioSource
local SageAudioSource = {}
function SageAudioSource:Play() end
function SageAudioSource:Stop() end
function SageAudioSource:Pause() end
function SageAudioSource:Resume() end
---@return boolean
function SageAudioSource:IsPlaying() end
---@param volume number
function SageAudioSource:SetVolume(volume) end
---@param pitch number
function SageAudioSource:SetPitch(pitch) end
---@param loop boolean
function SageAudioSource:SetLoop(loop) end
---@param clip string
function SageAudioSource:SetClip(clip) end
---Разовый звук ПОВЕРХ текущего: шаг, щелчок, попадание.
---@param clip string
---@param volume? number
function SageAudioSource:PlayOneShot(clip, volume) end

---@class SageCamera
---@field transform SageTransform
local SageCamera = {}
---@param fov number
function SageCamera:SetFOV(fov) end
---@return number
function SageCamera:GetFOV() end
---@param value number
function SageCamera:SetNearClip(value) end
---@param value number
function SageCamera:SetFarClip(value) end
---@param primary boolean
function SageCamera:SetPrimary(primary) end

---Ввод. Клавиша — для прототипа; игра, которую можно переназначить,
---спрашивает ДЕЙСТВИЕ.
Input = {}
---@param key string
---@return boolean
function Input:IsKeyDown(key) end
---@param key string
---@return boolean
function Input:IsKeyPressed(key) end
---@param button string "MOUSE_LEFT", "MOUSE_RIGHT", …
---@return boolean
function Input:IsMouseButtonDown(button) end
---@param button string
---@return boolean
function Input:IsMouseButtonPressed(button) end
---@return Vec2
function Input:GetMouseDelta() end
---@return Vec2
function Input:GetMousePosition() end
---@return number
function Input:GetMouseWheel() end
---@param action string
---@return boolean
function Input:IsActionDown(action) end
---@param action string
---@return boolean
function Input:IsActionPressed(action) end
---@param action string
---@return boolean
function Input:IsActionReleased(action) end
---@param action string
---@return number
function Input:GetAxis(action) end
---@param action string
---@return Vec2
function Input:GetVector2(action) end
---Объявить действие и клавишу: раскладка принадлежит игре, а не движку.
---@param action string
---@param source string
---@return boolean
function Input:BindAction(action, source) end
---@param captured boolean
function Input:SetCursorCaptured(captured) end
---@return boolean
function Input:IsCursorCaptured() end

---Время кадра. timeScale пишется: замедление — обычный приём игры.
---@class SageTime
---@field deltaTime number
---@field fixedDeltaTime number
---@field time number
---@field unscaledTime number
---@field timeScale number
Time = {}

---Отладочный вывод и отладочная графика.
Debug = {}
---@param message string
function Debug:Log(message) end
---@param message string
function Debug:Warning(message) end
---@param message string
function Debug:Error(message) end
function Debug:DrawLine(from, to) end
function Debug:DrawRay(origin, direction) end
function Debug:DrawSphere(center, radius) end

---Сцена. Прежний раздел sage.scene (Load и прочее) доступен через этот же
---объект.
Scene = {}
scene = Scene
---@param name string
---@return SageObject|nil
function Scene:Find(name) end
---@param id integer
---@return SageObject|nil
function Scene:FindById(id) end
---@param tag string
---@return SageObject|nil
function Scene:FindByTag(tag) end
---@param tag string
---@return SageObject[]
function Scene:FindAllByTag(tag) end
---@param name string
---@return SageObject
function Scene:Create(name) end
---Ставит префаб. Точку задают сразу: иначе объект мигает в начале координат.
---@param prefab string
---@param position? Vec3
---@return SageObject|nil
function Scene:Instantiate(prefab, position) end
---@param target SageObject|integer
function Scene:Destroy(target) end
---@return string
function Scene:Name() end

---Физические запросы.
Physics = {}
---@param origin Vec3
---@param direction Vec3
---@param maxDistance? number
---@return table|nil {object, point, normal, distance}
function Physics:Raycast(origin, direction, maxDistance) end
---@param center Vec3
---@param radius number
---@return SageObject[]
function Physics:OverlapSphere(center, radius) end
---@param gravity Vec3
function Physics:SetGravity(gravity) end

---Шина событий сцены — одна на кнопку интерфейса, скрипт и код на C++.
Events = {}
---@param name string
---@param arg? any
function Events:Emit(name, arg) end
---@param name string
---@param handler fun(arg: any)
---@return integer номер подписки
function Events:On(name, handler) end
---@param name string
---@param handler fun(arg: any)
---@return integer
function Events:Once(name, handler) end
---@param subscription integer
function Events:Off(subscription) end

---Звук без объекта: щелчок интерфейса, взрыв в точке мира.
Audio = {}
---@param clip string
---@param volume? number
function Audio.Play(clip, volume) end
---@param clip string
---@param position Vec3
---@param volume? number
function Audio.PlayAt(clip, position, volume) end
---@param volume number
function Audio.SetMasterVolume(volume) end

---Vector3 — не второй тип, а конструктор и статические функции поверх Vec3.
---@overload fun(x: number, y: number, z: number): Vec3
Vector3 = {}
---@param a Vec3
---@param b Vec3
---@return number
function Vector3.Dot(a, b) end
---@param a Vec3
---@param b Vec3
---@return Vec3
function Vector3.Cross(a, b) end
---@param v Vec3
---@return Vec3
function Vector3.Normalize(v) end
---@param v Vec3
---@return number
function Vector3.Length(v) end
---@param a Vec3
---@param b Vec3
---@return number
function Vector3.Distance(a, b) end
---@param a Vec3
---@param b Vec3
---@param t number
---@return Vec3
function Vector3.Lerp(a, b, t) end
---@param from Vec3
---@param to Vec3
---@param maxDelta number
---@return Vec3
function Vector3.MoveTowards(from, to, maxDelta) end

---@overload fun(x: number, y: number): Vec2
Vector2 = {}
---@overload fun(r: number, g: number, b: number, a?: number): Vec4
Color = {}
---@overload fun(): Vec4
Quaternion = {}

-- --- Хуки скрипта нового стиля --------------------------------------------
-- Объявляются в таблице, которую файл ВОЗВРАЩАЕТ. Ни один не обязателен:
-- зовётся только то, что скрипт объявил.
---@class Script
---@field gameObject SageObject
---@field transform SageTransform
local Script = {}
function Script:Start() end
---@param dt number
function Script:Update(dt) end
---@param dt number постоянный шаг
function Script:FixedUpdate(dt) end
---@param dt number
function Script:LateUpdate(dt) end
function Script:OnEnable() end
function Script:OnDisable() end
function Script:OnDestroy() end
---@param other SageObject
function Script:OnCollisionEnter(other) end
---@param other SageObject
function Script:OnCollisionExit(other) end
---@param other SageObject
function Script:OnTriggerEnter(other) end
---@param other SageObject
function Script:OnTriggerExit(other) end
---@param name string
function Script:OnAnimationEvent(name) end

'''

HOOKS = '''
-- --- Хуки скрипта ---------------------------------------------------------
-- Объявляются в файле скрипта; движок зовёт их сам. Ни один не обязателен.

---Один раз при старте.
---@param entity Entity
function OnStart(entity) end

---Каждый кадр. dt — секунд с прошлого кадра.
---@param entity Entity
---@param dt number
function OnUpdate(entity, dt) end

---Сообщение от другого скрипта (sage.msg.Send/Broadcast).
---@param entity Entity
---@param name string
---@param data any
function OnMessage(entity, name, data) end
'''


def render(modules, legacy):
    out = [HEADER]
    out.append('-- --- Модули ---------------------------------------------------------------\n')
    out.append('---@class sage\n')
    for module in sorted(modules):
        out.append(f'---@field {module} sage.{module}\n')
    out.append('sage = {}\n\n')

    for module in sorted(modules):
        out.append(f'---@class sage.{module}\n')
        out.append(f'sage.{module} = {{}}\n\n')
        out.append(f'---Короткое имя того же модуля: `{module}.X()` — это ТОТ ЖЕ объект,\n')
        out.append(f'---что `sage.{module}`, а не копия. Движок ставит его, только если имя\n')
        out.append('---свободно: игра, объявившая своё, остаётся при своём.\n')
        out.append(f'---@type sage.{module}\n{module} = nil\n\n')
        for name, params, rtype in modules[module]:
            for pname, ptype, opt in params:
                out.append(f'---@param {pname}{"?" if opt else ""} {ptype}\n')
            if rtype != 'nil':
                out.append(f'---@return {rtype}\n')
            arglist = ', '.join(p[0] for p in params)
            out.append(f'function sage.{module}.{name}({arglist}) end\n')
        out.append('\n')

    out.append('-- --- Старые глобальные имена ----------------------------------------------\n')
    out.append('-- Те же функции, что выше, под именами, с которых начинался движок.\n')
    out.append('-- Работают и работать будут: на них написаны существующие игры.\n')
    for old, module, name in legacy:
        out.append(f'---@type fun(...): any\n{old} = sage.{module}.{name}\n')
    out.append(HOOKS)
    return ''.join(out)


def main():
    modules, legacy = collect()
    text = render(modules, legacy)
    check = '--check' in sys.argv
    total = sum(len(v) for v in modules.values())
    if check:
        if not OUT.exists() or OUT.read_text(encoding='utf-8') != text:
            print(f"Подсказка {OUT.relative_to(ROOT)} устарела: движок регистрирует "
                  f"{total} функций в {len(modules)} модулях, а в файле лежит другое.\n"
                  f"Перезапишите её: python3 scripts/gen_script_api.py")
            return 1
        print(f"Подсказка по API свежая: функций {total}, модулей {len(modules)}.")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding='utf-8')
    print(f"{OUT.relative_to(ROOT)}: функций {total}, модулей {len(modules)}, "
          f"старых имён {len(legacy)}")
    return 0


if __name__ == '__main__':
    sys.exit(main())

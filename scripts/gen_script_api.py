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

---Вектор. Складывается, вычитается, умножается на число с любой стороны.
---@class Vec3
---@field x number
---@field y number
---@field z number
---@operator add(Vec3): Vec3
---@operator sub(Vec3): Vec3
---@operator mul(number): Vec3
---@operator div(number): Vec3
---@operator unm: Vec3
Vec3 = {}
---@return number
function Vec3:length() end
---@return Vec3
function Vec3:normalized() end
---@param other Vec3
---@return number
function Vec3:distance(other) end
---@param other Vec3
---@return number
function Vec3:dot(other) end
---@param other Vec3
---@return Vec3
function Vec3:cross(other) end
---Свой вектор-копия: менять его, не задевая исходный.
---@return Vec3
function Vec3:copy() end
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

---Место объекта в мире. `position.z = 5` пишет прямо в объект.
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
-- Нынешние имена тех же методов.
function SageTransform:set_position(x, y, z) end
function SageTransform:set_rotation(x, y, z) end
function SageTransform:set_scale(x, y, z) end
---@param delta Vec3
function SageTransform:translate(delta) end
function SageTransform:rotate(x, y, z) end
---@return Vec3
function SageTransform:forward() end
---@return Vec3
function SageTransform:right() end
---@return Vec3
function SageTransform:up() end
---@param target Vec3
function SageTransform:look_at(target) end
---@return Vec3
function SageTransform:world_position() end

---Объект сцены. Встроенные сигналы — свойства (`button.clicked`,
---`crate.collision`): их объявляют компоненты объекта. Интерфейс — часть
---объекта: text, visible, enabled, value (docs/scripting.md, «Сигналы и связи»).
---@class SageObject
---@field name string
---@field id integer
---@field tag string метка объекта (TagComponent)
---@field active boolean включён (виден и участвует в кадре)
---@field transform SageTransform
---@field parent SageObject|nil
---@field children SageObject[]
---@field clicked SageSignal|nil
---@field pressed SageSignal|nil
---@field released SageSignal|nil
---@field hovered SageSignal|nil
---@field unhovered SageSignal|nil
---@field value_changed SageSignal|nil
---@field text_changed SageSignal|nil
---@field collision SageSignal|nil
---@field collision_ended SageSignal|nil
---@field trigger_entered SageSignal|nil
---@field trigger_exited SageSignal|nil
---@field signals string[] сигналы, объявленные компонентами объекта
---@field text string|nil надпись (своя или первой дочерней надписи)
---@field visible boolean показан ли (у элемента интерфейса — Active)
---@field enabled boolean ловит ли мышь (Interactable)
---@field value number|boolean|nil значение ползунка или галки
local SageObject = {}
---Добавить компонент кодом. Для "Script" вернёт таблицу скрипта (Start уже позван).
---"RigidBody": {type="static|dynamic|kinematic", mass, friction, restitution, sensor};
---"Collider": {shape="box|sphere|capsule", size=Vec3, radius, halfHeight};
---"Script": {path="assets/..."}; "CharacterController";
---"Light": {type="point|spot|directional", intensity, range, color}; "Camera": {fov, primary};
---"Mesh": {color}.
---@param name string
---@param opts? table
---@return any
function SageObject:AddComponent(name, opts) end
---@param name string
---@return boolean
function SageObject:HasComponent(name) end
---@param name string
---@return boolean снят ли
function SageObject:RemoveComponent(name) end
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

---Сигнал по имени, в том числе свой: `player:signal("player_died")`.
---@param name string
---@return SageSignal
function SageObject:signal(name) end
---Подписаться на сигнал этого объекта: `player:on("player_died", fn)`.
---@param name string
---@param handler fun(event: SageEvent, data: any)
---@return SageConnection
function SageObject:on(name, handler) end
---@param name string
---@param handler fun(event: SageEvent, data: any)
---@return SageConnection
function SageObject:once(name, handler) end
---Послать сигнал этого объекта: `player:emit("player_died", {score = 10})`.
---@param name string
---@param data? any
function SageObject:emit(name, data) end
---Снять СВОИ (скриптовые) подписки на сигнал; вернёт их число.
---@param name string
---@return integer
function SageObject:off(name) end
---Любое свойство по имени (как у Tween.to): obj:get("opacity").
---@param property string
---@return any
function SageObject:get(property) end
---@param property string
---@param value any
function SageObject:set(property, value) end
-- Нынешние имена тех же методов.
---Позвать функцию скрипта объекта: `player:call("TakeDamage", 10)`.
---@param method string
---@return any
function SageObject:call(method, ...) end
---@param name string
---@return any|nil
function SageObject:get_component(name) end
---@param name string
---@param options? table
---@return any
function SageObject:add_component(name, options) end
---@param name string
---@return boolean
function SageObject:has_component(name) end
function SageObject:destroy() end
---@return table|nil
function SageObject:get_script() end
---@param parent SageObject|nil
function SageObject:set_parent(parent) end
---@return boolean
function SageObject:is_valid() end
---@param text string
function SageObject:set_text(text) end
function SageObject:show() end
function SageObject:hide() end
---@param on boolean
function SageObject:set_visible(on) end
---@param on boolean
function SageObject:set_enabled(on) end

---Что получает обработчик сигнала первым аргументом.
---@class SageEvent
---@field name string имя сигнала
---@field sender SageObject|nil кто послал
---@field data any что послали (оно же — второй аргумент)

---Сигнал объекта (или глобальный — Events.signal).
---@class SageSignal
---@field name string
---@field object SageObject|nil
local SageSignal = {}
---@param handler fun(event: SageEvent, data: any)
---@return SageConnection
function SageSignal:connect(handler) end
---Сработает один раз и снимется сам.
---@param handler fun(event: SageEvent, data: any)
---@return SageConnection
function SageSignal:once(handler) end
---@param data? any
function SageSignal:emit(data) end
---Снять все подписки ИЗ СКРИПТОВ на этот сигнал; вернёт их число.
---@return integer
function SageSignal:disconnect_all() end
---Сколько подписчиков (включая связи инспектора и C++).
---@return integer
function SageSignal:count() end

---То, что вернул connect.
---@class SageConnection
---@field connected boolean жива ли подписка
---@field id integer
local SageConnection = {}
---Снять подписку. Повторно — безопасно: вернёт false.
---@return boolean
function SageConnection:disconnect() end

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
---Секунд с прошлого кадра (с учётом масштаба времени).
---@return number
function Time.delta() end
---Шаг FixedUpdate.
---@return number
function Time.fixed_delta() end
---Секунд с начала игры.
---@return number
function Time.total() end
---@return number
function Time.scale() end
---@param scale number 0 — пауза, 0.5 — замедление
function Time.set_scale(scale) end

---Отладочный вывод и отладочная графика.
Debug = {}
---@param message string
function Debug:Log(message) end
---@param message string
function Debug:Warning(message) end
---@param message string
function Debug:Error(message) end
---Отладочная линия. Цвет и длительность необязательны; без длительности линия
---живёт ОДИН кадр (её перезаказывают, пока она нужна).
---@param from Vec3
---@param to Vec3
---@param color? Vec3
---@param duration? number
function Debug:DrawLine(from, to, color, duration) end
---Длина берётся из самого направления.
---@param origin Vec3
---@param direction Vec3
---@param color? Vec3
---@param duration? number
function Debug:DrawRay(origin, direction, color, duration) end
---@param center Vec3
---@param radius number
---@param color? Vec3
---@param duration? number
function Debug:DrawSphere(center, radius, color, duration) end
---@param center Vec3
---@param halfExtents Vec3
---@param color? Vec3
---@param duration? number
function Debug:DrawBox(center, halfExtents, color, duration) end

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
---Перейти на другой уровень. Имя — без папки и расширения ("level2").
---ЗАПРОС: выполнит его движок между кадрами, когда ни один скрипт не идёт.
---@param name string
function Scene:Load(name) end
---Начать этот же уровень заново.
function Scene:Reload() end

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

---Глобальные события: сигналы «ничьего» объекта. Та же шина, что у сигналов
---объектов, связей инспектора и кода на C++.
Events = {}
---@param name string
---@param handler fun(event: SageEvent, data: any)
---@return SageConnection
function Events.on(name, handler) end
---@param name string
---@param handler fun(event: SageEvent, data: any)
---@return SageConnection
function Events.once(name, handler) end
---@param name string
---@param data? any
function Events.emit(name, data) end
---Снять: соединение, его номер или все свои подписки на имя.
---@param what SageConnection|integer|string
---@return integer
function Events.off(what) end
---@param name string
---@return integer
function Events.count(name) end
---@param name string
---@return SageSignal
function Events.signal(name) end
---Прежние имена — те же функции.
Events.On = Events.on
Events.Once = Events.once
Events.Emit = Events.emit
Events.Off = Events.off
Events.Count = Events.count

---Интерфейс по имени элемента.
UI = {}
---Элемент по имени; нет такого — понятная ошибка.
---@param name string
---@return SageObject
function UI.get(name) end
---Элемент по имени или nil.
---@param name string
---@return SageObject|nil
function UI.find(name) end
---Создать элемент по пресету ("Button", "Label", "Slider", "Checkbox", ...).
---@param preset string
---@param name? string
---@param parent? SageObject
---@return SageObject
function UI.create(preset, name, parent) end

---Lua-тесты: файл *.test.lua возвращает таблицу с методами test_*; они идут
---в настоящем Play по одному (Play → «Запустить Lua-тесты»). before_each и
---after_each оборачивают каждый тест. Ожидания — в кадрах игры.
Test = {}
---@param cond any
---@param msg? string
function Test.expect(cond, msg) end
---@param actual any
---@param expected any
---@param msg? string
function Test.eq(actual, expected, msg) end
---@param actual any
---@param other any
---@param msg? string
function Test.ne(actual, other, msg) end
---@param actual number
---@param expected number
---@param eps? number
---@param msg? string
function Test.near(actual, expected, eps, msg) end
---@param msg? string
function Test.fail(msg) end
---Функция обязана упасть; needle — кусок текста ошибки.
---@param fn function
---@param needle? string
---@param msg? string
function Test.errors(fn, needle, msg) end
---Ждать секунды игрового времени (без учёта timeScale).
---@param seconds number
function Test.wait(seconds) end
---@param n? integer
function Test.frames(n) end
---Ждать, пока условие не станет истинным (по умолчанию до 5 с).
---@param pred fun(): boolean
---@param timeout? number
---@param msg? string
function Test.waitUntil(pred, timeout, msg) end
---Предел времени этого теста (по умолчанию 30 с).
---@param seconds number
function Test.timeout(seconds) end
---@return number секунды с начала теста
function Test.elapsed() end
---@param ... any
function Test.log(...) end
---Щелчок мышью по элементу интерфейса (настоящий путь ввода UI).
---false — элемента на экране нет (спрятан, выключен, за краем).
---@param element SageObject|string
---@return boolean
function Test.click(element) end
---@param element SageObject|string
---@param text string
function Test.type(element, text) end
---Ползунок: t — доля 0..1 по ширине.
---@param element SageObject|string
---@param t number
function Test.slide(element, t) end
---Навести курсор; nil — увести (unhovered).
---@param element SageObject|string|nil
function Test.hover(element) end
---Клавиша: down=false — отпустить. Видна со следующего кадра.
---@param name string
---@param down? boolean
function Test.key(name, down) end

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

-- --- Нынешние имена разделов ---------------------------------------------
-- У каждой функции раздела есть имя через подчёркивание: Input.IsKeyDown —
-- это Input.is_key_down. Ниже — те, которыми пишут простые скрипты.

---@param key string "W", "Space", "E", "Left", "F1"…
---@return boolean держится прямо сейчас
function Input.is_key_down(key) end
---@param key string
---@return boolean нажата ИМЕННО в этом кадре
function Input.is_key_pressed(key) end
---@param key string
---@return boolean отпущена ИМЕННО в этом кадре
function Input.is_key_released(key) end
---@param button string|integer "left", "right", "middle" или 0/1/2
---@return boolean
function Input.is_mouse_down(button) end
---@param button string|integer
---@return boolean
function Input.is_mouse_pressed(button) end
---@param button string|integer
---@return boolean
function Input.is_mouse_released(button) end
---@return Vec2
function Input.mouse_position() end
---@return Vec2
function Input.mouse_delta() end
---@return number
function Input.mouse_wheel() end
---@param action string
---@return boolean
function Input.is_action_down(action) end
---@param action string
---@return boolean
function Input.is_action_pressed(action) end
---@param action string
---@return number
function Input.get_axis(action) end

---@param message any
function Debug.log(message) end
---@param message any
function Debug.warn(message) end
---@param message any
function Debug.error(message) end

---Объект по имени; нет — nil.
---@param name string
---@return SageObject|nil
function Scene.find(name) end
---Все объекты с таким именем ИЛИ меткой.
---@param name_or_tag string
---@return SageObject[]
function Scene.find_all(name_or_tag) end
---@param name string
---@return SageObject
function Scene.create(name) end
---@param prefab string
---@param position? Vec3
---@return SageObject|nil
function Scene.instantiate(prefab, position) end
---@param target SageObject|integer
function Scene.destroy(target) end
---@param name string
function Scene.load(name) end

---@param origin Vec3
---@param direction Vec3
---@param max_distance? number
---@return table|nil {object, point, normal, distance}
function Physics.raycast(origin, direction, max_distance) end
---@param center Vec3
---@param radius number
---@return SageObject[]
function Physics.overlap_sphere(center, radius) end

---@param clip string
---@param volume? number
function Audio.play(clip, volume) end
---@param clip string
---@param position Vec3
---@param volume? number
function Audio.play_at(clip, position, volume) end

-- --- Твины ----------------------------------------------------------------
-- Быстрое изменение свойства за время. Тот же проигрыватель, что у окна Tween
-- редактора (docs/scripting.md, «Твины»).

---Кривые: Ease.Linear, Ease.In, Ease.Out, Ease.InOut, Ease.OutBack, Ease.InOutSine…
---Своя кривая — таблица {x1, y1, x2, y2} (кубическая Безье, как в CSS).
---@class SageEase
---@field Linear string
---@field In string
---@field Out string
---@field InOut string
---@field InQuad string
---@field OutQuad string
---@field InOutQuad string
---@field InCubic string
---@field OutCubic string
---@field InOutCubic string
---@field InQuart string
---@field OutQuart string
---@field InOutQuart string
---@field InQuint string
---@field OutQuint string
---@field InOutQuint string
---@field InSine string
---@field OutSine string
---@field InOutSine string
---@field InExpo string
---@field OutExpo string
---@field InOutExpo string
---@field InCirc string
---@field OutCirc string
---@field InOutCirc string
---@field InBack string
---@field OutBack string
---@field InOutBack string
---@field InElastic string
---@field OutElastic string
---@field InOutElastic string
---@field InBounce string
---@field OutBounce string
---@field InOutBounce string
Ease = {}

---@alias SageEaseValue string|number[]

---Запущенный твин. Методы цепочки возвращают его же.
---@class SageTween
local SageTween = {}
---Затем: следующий шаг начинается, когда кончился предыдущий.
---@param property string|table
---@param value any
---@param duration number
---@param ease? SageEaseValue
---@return SageTween
function SageTween:then_to(property, value, duration, ease) end
---Вместе: шаг идёт одновременно с предыдущим.
---@param property string|table
---@param value any
---@param duration number
---@param ease? SageEaseValue
---@return SageTween
function SageTween:with(property, value, duration, ease) end
---Пауза в последовательности.
---@param seconds number
---@return SageTween
function SageTween:wait(seconds) end
---Задержка перед стартом.
---@param seconds number
---@return SageTween
function SageTween:delay(seconds) end
---Повторять: "loop" (по умолчанию), "pingpong", "once".
---@param mode? string
---@return SageTween
function SageTween:loop(mode) end
---@return SageTween
function SageTween:ping_pong() end
---@param k number 2 — вдвое быстрее
---@return SageTween
function SageTween:speed(k) end
---@param on? boolean
---@return SageTween
function SageTween:reverse(on) end
---Одна кривая всем шагам.
---@param ease SageEaseValue
---@return SageTween
function SageTween:ease(ease) end
---@param fn fun()
---@return SageTween
function SageTween:on_complete(fn) end
function SageTween:pause() end
function SageTween:resume() end
function SageTween:cancel() end
---@return boolean
function SageTween:is_playing() end
---@return number секунды от начала
function SageTween:time() end
---@return number длина одного прохода
function SageTween:duration() end

---Твины: Tween.to(self, "position", Vector3.new(5, 2, 0), 1.0, Ease.Out).
Tween = {}
---Довести свойство (или несколько: {position = …, scale = …}) до значения.
---Свойства: position, rotation, scale, color, opacity, size, text_size,
---corner_radius, intensity, fov, volume — или полный ключ ("fill.color").
---@param target SageObject|table объект или self
---@param property string|table
---@param value any число, Vector3, Vector2, Color
---@param duration number
---@param ease? SageEaseValue по умолчанию Ease.Out
---@return SageTween
function Tween.to(target, property, value, duration, ease) end
---Из значения — к тому, что есть сейчас: «появиться из 0».
---@param target SageObject|table
---@param property string|table
---@param value any
---@param duration number
---@param ease? SageEaseValue
---@return SageTween
function Tween.from(target, property, value, duration, ease) end
---Пустая последовательность: шаги — :then_to / :with / :wait.
---@param target SageObject|table
---@return SageTween
function Tween.sequence(target) end
---Твин, собранный в окне Tween редактора, по имени.
---@param target SageObject|table
---@param name string
---@return SageTween|nil
function Tween.play(target, name) end
---Снять: твин или все твины объекта (со свойством — только его).
---@param what SageTween|SageObject|table
---@param property? string
---@return integer
function Tween.cancel(what, property) end
---@param what SageTween|SageObject|table
function Tween.pause(what) end
---@param what SageTween|SageObject|table
function Tween.resume(what) end
---@param what SageTween|SageObject|table
---@return boolean
function Tween.is_playing(what) end
---@return integer
function Tween.count() end

---Vector3 — не второй тип, а конструктор и статические функции поверх Vec3.
---@overload fun(x: number, y: number, z: number): Vec3
Vector3 = {}
---@param x number
---@param y number
---@param z number
---@return Vec3
function Vector3.new(x, y, z) end
---Новый вектор (0, 0, 0) — свой, его можно менять.
---@return Vec3
function Vector3.zero() end
---@return Vec3
function Vector3.one() end
---@return Vec3
function Vector3.up() end
---@return Vec3
function Vector3.down() end
---@return Vec3
function Vector3.right() end
---@return Vec3
function Vector3.left() end
---@return Vec3
function Vector3.forward() end
---@return Vec3
function Vector3.back() end
---@param a Vec3
---@param b Vec3
---@return number
function Vector3.distance(a, b) end
---@param a Vec3
---@param b Vec3
---@return number
function Vector3.dot(a, b) end
---@param a Vec3
---@param b Vec3
---@return Vec3
function Vector3.cross(a, b) end
---@param v Vec3
---@return Vec3
function Vector3.normalize(v) end
---@param v Vec3
---@return number
function Vector3.length(v) end
---@param a Vec3
---@param b Vec3
---@param t number
---@return Vec3
function Vector3.lerp(a, b, t) end
---@param from Vec3
---@param to Vec3
---@param max_delta number
---@return Vec3
function Vector3.move_towards(from, to, max_delta) end
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

-- --- ПРОСТОЙ СКРИПТ: набор функций ---------------------------------------
--
--     speed = 5.0                      -- поле инспектора (тип — по значению)
--     function Start() Debug.log(self.name) end
--     function Update(dt) self.transform.position.z = self.transform.position.z + speed * dt end
--     function OnCollisionEnter(other) Debug.log("Hit " .. other.name) end
--
-- Ни одна функция не обязательна: движок зовёт только объявленные. Своё
-- событие: Events.emit("door_open") зовёт OnDoorOpen(data) у всех скриптов.

---Свой объект скрипта.
---@class Script
---@field name string имя объекта
---@field transform SageTransform
---@field game_object SageObject
---@field gameObject SageObject
local Script = {}
---@param name string "Transform", "Audio", "Camera", "Animation", "CharacterController", "Script"
---@return any|nil
function Script:get_component(name) end
---@param name string
---@param options? table
---@return any
function Script:add_component(name, options) end
---@param name string
---@return boolean
function Script:has_component(name) end
---Удалить свой объект.
function Script:destroy() end

---Сам объект, на котором стоит скрипт.
---@type Script
self = nil

---Один раз, когда объект ожил (после того как подключены все скрипты сцены).
function Start() end
---Каждый кадр.
---@param dt number секунд с прошлого кадра
function Update(dt) end
---С постоянным шагом — для физики.
---@param dt number
function FixedUpdate(dt) end
---После всех Update кадра — камера за игроком.
---@param dt number
function LateUpdate(dt) end
---Перед удалением объекта или скрипта.
function Destroy() end
---@param other SageObject
function OnCollisionEnter(other) end
---@param other SageObject
function OnCollisionExit(other) end
---@param other SageObject
function OnTriggerEnter(other) end
---@param other SageObject|nil
function OnTriggerExit(other) end
---Клавиша нажата в этом кадре.
---@param key string "W", "Space", …
function OnKeyDown(key) end
---Клавиша отпущена в этом кадре.
---@param key string
function OnKeyUp(key) end

-- --- Хуки скрипта-таблицы -------------------------------------------------
-- Объявляются в таблице, которую файл ВОЗВРАЩАЕТ (`return Player`).
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
---@param other SageObject|nil  nil — гостя удалили, пока он был в зоне
function Script:OnTriggerExit(other) end
---@param other SageObject
function Script:OnTriggerStay(other) end
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

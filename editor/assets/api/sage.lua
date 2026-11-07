---@meta
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
---Куда смотрит объект — локальный −Z, то же направление, что у камеры.
---@return Vec3
function SageTransform:Forward() end
---@return Vec3
function SageTransform:Right() end
---@return Vec3
function SageTransform:Up() end
---Разворачивает Forward() (и камеру на объекте) к цели.
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
---Перенести компонент целиком на другой объект (сейчас — "Camera", вместе с
---её пост-обработкой). Персонажу от первого лица: камеру с тела — на «взгляд».
---@param name string
---@param target SageObject
---@return boolean перенесён ли
function SageObject:MoveComponent(name, target) end
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
---Объект переходит в следующую сцену при scene:Load (вместе с потомками).
---@param on? boolean по умолчанию true
function SageObject:KeepOnLoad(on) end
---@return boolean
function SageObject:IsKeptOnLoad() end
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
---Слой персонажа — маской битов (бит 0 — слой 1), как у RigidBody.
---@param mask integer
function SageCharacterController:SetLayer(mask) end
---@return integer
function SageCharacterController:GetLayer() end
---С какими слоями сталкивается (маска битов).
---@param mask integer
function SageCharacterController:SetCollidesWith(mask) end
---@return integer
function SageCharacterController:GetCollidesWith() end
---Слать ли OnCollisionEnter/Exit о касаниях персонажа.
---@param on boolean
function SageCharacterController:SetCollisionEvents(on) end
---Касается ли персонаж объекта сейчас (пол, стена, ящик).
---@param other SageObject
---@return boolean
function SageCharacterController:IsTouching(other) end
---@return integer
function SageCharacterController:GetTouchCount() end

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
---Действие-вектор из четырёх клавиш (y — вперёд, x — вправо). Не трогает
---действие, у которого уже есть привязки (раскладка проекта главнее).
---@param action string
---@param up string
---@param down string
---@param left string
---@param right string
---@return boolean
function Input:BindVector(action, up, down, left, right) end
---Ось из двух клавиш: positive даёт +1, negative — −1.
---@param action string
---@param positive string
---@param negative string
---@return boolean
function Input:BindAxis(action, positive, negative) end
---@param action string
---@return boolean
function Input:HasAction(action) end
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
---Настоящие часы в секундах (монотонные): делить тяжёлую работу по кадрам.
---@return number
function Time.clock() end
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

---Плагины: обмен API между плагином (plugins/<id>/runtime.lua) и скриптами
---игры. Плагин регистрирует таблицу в теле файла, игра берёт её по id.
plugins = {}
Plugins = plugins
---@param id string имя плагина
---@param api table то, что получат скрипты игры
function plugins.Register(id, api) end
---@param id string
---@return table|nil
function plugins.Get(id) end
---@param id string
---@return boolean
function plugins.Has(id) end
---@return string[]
function plugins.List() end

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
---params — таблица, которую новая сцена получит через scene:Params().
---@param name string
---@param params? table
function Scene:Load(name, params) end
---Начать этот же уровень заново.
function Scene:Reload() end
---Параметры, с которыми пришли в эту сцену (пустая таблица — без них).
---@return table
function Scene:Params() end
---Из какой сцены пришли ("" — эта первая).
---@return string
function Scene:Previous() end
---Есть ли сцена с таким именем.
---@param name string
---@return boolean
function Scene:Exists(name) end
---Имена всех сцен игры.
---@return string[]
function Scene:List() end
---Добавить содержимое другой сцены в эту (общий интерфейс, кусок карты).
---Возвращает корни добавленного; убирается UnloadAdditive(группа).
---@param name string
---@param offset? Vec3
---@param group? string группа; по умолчанию — имя сцены
---@return SageObject[]|nil
function Scene:LoadAdditive(name, offset, group) end
---Убрать всё, что добавлено группой. Возвращает число снятых корней.
---@param group string
---@return integer
function Scene:UnloadAdditive(group) end

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
---(0, 0, −1) — как Transform:Forward() без поворота.
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

-- --- Модули ---------------------------------------------------------------
---@class sage
---@field ai sage.ai
---@field anim sage.anim
---@field app sage.app
---@field audio sage.audio
---@field camera sage.camera
---@field core sage.core
---@field events sage.events
---@field fx sage.fx
---@field game sage.game
---@field ik sage.ik
---@field input sage.input
---@field lensflare sage.lensflare
---@field light sage.light
---@field material sage.material
---@field math sage.math
---@field mesh sage.mesh
---@field msg sage.msg
---@field noise sage.noise
---@field physics sage.physics
---@field post sage.post
---@field reflect sage.reflect
---@field render sage.render
---@field rt sage.rt
---@field save sage.save
---@field scene sage.scene
---@field texture sage.texture
---@field time sage.time
---@field tween sage.tween
---@field ui sage.ui
---@field user_folder sage.user_folder
---@field visual sage.visual
---@field volumetric sage.volumetric
sage = {}

---@class sage.ai
sage.ai = {}

---Короткое имя того же модуля: `ai.X()` — это ТОТ ЖЕ объект,
---что `sage.ai`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.ai
ai = nil

---@param owner any
---@param opts? table
---@return table
function sage.ai.agent(owner, opts) end
---@param entity any
---@return any
function sage.ai.agent_of(entity) end
---@param name string
---@param ...? ...
---@return any
function sage.ai.call(name, ...) end
---@param fn function
function sage.ai.connect_rule(fn) end
---@param fn function
function sage.ai.convert_rule(fn) end
---@param event string
---@param data any
---@param target any
---@return integer
function sage.ai.emit(event, data, target) end
---@param path string
---@return string
function sage.ai.graph(path) end
---@param dir string
---@return integer
function sage.ai.load_library(dir) end
---@param vars any
---@return table
function sage.ai.names(vars) end
---@param id string
---@param spec table
function sage.ai.node(id, spec) end
---@return table
function sage.ai.nodes() end
---@param name string
---@param fn function
---@param spec? table
function sage.ai.operation(name, fn, spec) end
---@return table
function sage.ai.operations() end
---@param fn function
function sage.ai.path_finder(fn) end
---@param space? string
---@return any
function sage.ai.shared(space) end
---@param name string
---@param spec table
function sage.ai.task(name, spec) end
---@return table
function sage.ai.tasks() end
---@param id string
---@param spec table
function sage.ai.type(id, spec) end

---@class sage.anim
sage.anim = {}

---Короткое имя того же модуля: `anim.X()` — это ТОТ ЖЕ объект,
---что `sage.anim`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.anim
anim = nil

---@param entity Entity
---@param path string
function sage.anim.Add(entity, path) end
---@param entity Entity
---@param bone string
---@param axis Vec3
---@return Vec3
function sage.anim.BoneAxis(entity, bone, axis) end
---@param entity Entity
---@param bone string
---@return Vec3
function sage.anim.BonePosition(entity, bone) end
---@param entity Entity
---@param path string
---@return integer
function sage.anim.Borrow(entity, path) end
---@param entity Entity
function sage.anim.ClearJointPoses(entity) end
---@param entity Entity
---@return integer
function sage.anim.Count(entity) end
---@param entity Entity
---@param clip integer
---@return number
function sage.anim.Duration(entity, clip) end
---@param entity Entity
---@return number
function sage.anim.Fade(entity) end
---@param entity Entity
---@param name string
---@return integer
function sage.anim.Index(entity, name) end
---@param entity Entity
---@param name string
---@return integer
function sage.anim.JointIndex(entity, name) end
---@param entity Entity
---@return table
function sage.anim.JointNames(entity) end
---@param entity Entity
---@return table
function sage.anim.Names(entity) end
---@param entity Entity
---@param clip integer
---@param blend? number
---@return boolean
function sage.anim.Play(entity, clip, blend) end
---@param entity Entity
---@param name string
---@param blend? number
---@return boolean
function sage.anim.PlayNamed(entity, name, blend) end
---@param entity Entity
---@param time number
function sage.anim.Seek(entity, time) end
---@param entity Entity
---@param joint integer
---@param x number
---@param y number
---@param z number
function sage.anim.SetJointOffset(entity, joint, x, y, z) end
---@param entity Entity
---@param joint integer
---@param rx number
---@param ry number
---@param rz number
function sage.anim.SetJointRotation(entity, joint, rx, ry, rz) end
---@param entity Entity
---@param loop boolean
function sage.anim.SetLoop(entity, loop) end
---@param entity Entity
---@param playing boolean
function sage.anim.SetPlaying(entity, playing) end
---@param entity Entity
---@param on boolean
function sage.anim.SetRootMotion(entity, on) end
---@param entity Entity
---@param speed number
function sage.anim.SetSpeed(entity, speed) end
---@param entity Entity
---@return number
function sage.anim.Time(entity) end

---@class sage.app
sage.app = {}

---Короткое имя того же модуля: `app.X()` — это ТОТ ЖЕ объект,
---что `sage.app`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.app
app = nil

---@param key string
---@return string
function sage.app.Arg(key) end
---@param key string
---@return boolean
function sage.app.Flag(key) end

---@class sage.audio
sage.audio = {}

---Короткое имя того же модуля: `audio.X()` — это ТОТ ЖЕ объект,
---что `sage.audio`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.audio
audio = nil

---@param seconds number
function sage.audio.FadeOutMusic(seconds) end
---@return boolean
function sage.audio.IsMusicPlaying() end
---@param entity Entity
---@return boolean
function sage.audio.IsPlaying(entity) end
---@param entity Entity
function sage.audio.Play(entity) end
---@param path string
---@param volume? number
---@param loop? boolean
---@param fade? number
function sage.audio.PlayMusic(path, volume, loop, fade) end
---@param path string
---@param volume? number
---@param pitch? number
function sage.audio.PlaySound(path, volume, pitch) end
---@param path string
---@param pos Vec3
---@param volume? number
---@param pitch? number
function sage.audio.PlaySound3D(path, pos, volume, pitch) end
---@param entity Entity
---@param clip string
function sage.audio.SetClip(entity, clip) end
---@param volume number
function sage.audio.SetMasterVolume(volume) end
---@param volume number
function sage.audio.SetMusicVolume(volume) end
---@param entity Entity
---@param pitch number
function sage.audio.SetPitch(entity, pitch) end
---@param entity Entity
---@param volume number
function sage.audio.SetVolume(entity, volume) end
---@param entity Entity
function sage.audio.Stop(entity) end
function sage.audio.StopMusic() end

---@class sage.camera
sage.camera = {}

---Короткое имя того же модуля: `camera.X()` — это ТОТ ЖЕ объект,
---что `sage.camera`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.camera
camera = nil

---@return any
function sage.camera.Get() end
---@param screenX number
---@param screenY number
---@return table
function sage.camera.ScreenToRay(screenX, screenY) end
---@param worldPos Vec3
---@return any
function sage.camera.WorldToScreen(worldPos) end
---@param entity Entity
---@return table
function sage.camera.passes(entity) end
---@param entity Entity
---@param passes table
function sage.camera.set_passes(entity, passes) end

---@class sage.core
sage.core = {}

---Короткое имя того же модуля: `core.X()` — это ТОТ ЖЕ объект,
---что `sage.core`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.core
core = nil

---@param message string
function sage.core.log(message) end
---@param message string
function sage.core.logError(message) end
---@param message string
function sage.core.logWarn(message) end

---@class sage.events
sage.events = {}

---Короткое имя того же модуля: `events.X()` — это ТОТ ЖЕ объект,
---что `sage.events`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.events
events = nil

---@param name string
---@return integer
function sage.events.Count(name) end
---@param name string
---@param payload any
function sage.events.Emit(name, payload) end
---@param arg any
function sage.events.Off(arg) end
---@param name string
---@param fn function
---@return integer
function sage.events.On(name, fn) end
---@param name string
---@param fn function
---@return integer
function sage.events.Once(name, fn) end

---@class sage.fx
sage.fx = {}

---Короткое имя того же модуля: `fx.X()` — это ТОТ ЖЕ объект,
---что `sage.fx`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.fx
fx = nil

---@param pos Vec3
---@param size Vec2
---@param texturePath? string
---@return integer
function sage.fx.AddBillboard(pos, size, texturePath) end
---@param id string
---@param fx any
---@param pos Vec3
function sage.fx.CreateStream(id, fx, pos) end
---@param fx any
---@param pos Vec3
---@param count integer
function sage.fx.Emit(fx, pos, count) end
---@param entity Entity
---@param count integer
function sage.fx.EmitFrom(entity, count) end
---@param path string
---@return any
function sage.fx.LoadEffect(path) end
---@param entity Entity
function sage.fx.Play(entity) end
---@param id integer
function sage.fx.RemoveBillboard(id) end
---@param id string
function sage.fx.RemoveStream(id) end
---@param id integer
---@param pos Vec3
function sage.fx.SetBillboardPosition(id, pos) end
---@param id integer
---@param tint Vec4
function sage.fx.SetBillboardTint(id, tint) end
---@param id integer
---@param visible boolean
function sage.fx.SetBillboardVisible(id, visible) end
---@param id string
---@param active boolean
function sage.fx.SetStreamActive(id, active) end
---@param id string
---@param pos Vec3
function sage.fx.SetStreamPosition(id, pos) end
---@param entity Entity
---@param clear? boolean
function sage.fx.Stop(entity, clear) end

---@class sage.game
sage.game = {}

---Короткое имя того же модуля: `game.X()` — это ТОТ ЖЕ объект,
---что `sage.game`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.game
game = nil

---@return boolean
function sage.game.HasPauseMenu() end
---@return boolean
function sage.game.IsPaused() end
---@param paused boolean
function sage.game.Pause(paused) end
function sage.game.Quit() end
function sage.game.Restart() end
---@param arg1 boolean
function sage.game.SetPauseMenu(arg1) end
---@param path? string
---@return string
function sage.game.screenshot(path) end

---@class sage.ik
sage.ik = {}

---Короткое имя того же модуля: `ik.X()` — это ТОТ ЖЕ объект,
---что `sage.ik`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.ik
ik = nil

---@param entity Entity
---@param bone string
---@param chainLength? integer
---@return integer
function sage.ik.AddGoal(entity, bone, chainLength) end
---@param entity Entity
---@param goal integer
---@return boolean
function sage.ik.Locked(entity, goal) end
---@param entity Entity
---@param goal integer
---@param axis Vec3
---@param maxAngle? number
function sage.ik.SetAim(entity, goal, axis, maxAngle) end
---@param entity Entity
---@param goal integer
---@param normal Vec3
function sage.ik.SetAlign(entity, goal, normal) end
---@param entity Entity
---@param on boolean
function sage.ik.SetEnabled(entity, on) end
---@param entity Entity
---@param goal integer
---@param on boolean
---@param plantHeight? number
---@param releaseTime? number
function sage.ik.SetFootLock(entity, goal, on, plantHeight, releaseTime) end
---@param entity Entity
---@param goal integer
---@param pole Vec3
function sage.ik.SetPole(entity, goal, pole) end
---@param entity Entity
---@param goal integer
---@param target Vec3
function sage.ik.SetTarget(entity, goal, target) end
---@param entity Entity
---@param goal integer
---@param weight number
function sage.ik.SetWeight(entity, goal, weight) end

---@class sage.input
sage.input = {}

---Короткое имя того же модуля: `input.X()` — это ТОТ ЖЕ объект,
---что `sage.input`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.input
input = nil

---@param action string
---@param source string
---@return boolean
function sage.input.AddBinding(action, source) end
---@return any
function sage.input.AnyPressedSource() end
---@param name string
---@return number
function sage.input.Axis(name) end
---@param action string
---@param keys any
---@return integer
function sage.input.Bind(action, keys) end
---@param action string
---@param positive string
---@param negative string
---@return integer
function sage.input.BindAxis(action, positive, negative) end
---@param context string
---@param action string
---@param keys any
---@return integer
function sage.input.BindIn(context, action, keys) end
---@param action string
---@param up string
---@param down string
---@param left string
---@param right string
function sage.input.BindVector(action, up, down, left, right) end
---@param action string
---@param settings table
---@return boolean
function sage.input.Configure(action, settings) end
---@return table
function sage.input.ContextNames() end
---@param name string
---@param priority integer
function sage.input.CreateContext(name, priority) end
---@param source string
---@return any
function sage.input.FindConflict(source) end
---@param index? integer
---@return boolean
function sage.input.GamepadConnected(index) end
---@param index? integer
---@return string
function sage.input.GamepadName(index) end
---@param name string
---@return boolean
function sage.input.Has(name) end
---@param name string
---@return boolean
function sage.input.IsContextEnabled(name) end
---@param name string
---@return boolean
function sage.input.IsDown(name) end
---@return boolean
function sage.input.IsMouseCaptured() end
---@param file string
---@return boolean
function sage.input.LoadMapping(file) end
---@return Vec2
function sage.input.MouseDelta() end
---@return Vec2
function sage.input.MousePosition() end
---@param action string
---@param source string
---@return boolean
function sage.input.Rebind(action, source) end
function sage.input.ReleaseAll() end
---@param file string
---@return boolean
function sage.input.SaveMapping(file) end
---@return number
function sage.input.ScrollDelta() end
---@param name string
---@param enabled boolean
function sage.input.SetContextEnabled(name, enabled) end
---@param captured boolean
function sage.input.SetMouseCaptured(captured) end
---@param action string
---@param mode string
---@return boolean
function sage.input.SetTrigger(action, mode) end
---@param source string
---@return boolean
function sage.input.SourceDown(source) end
---@param source string
---@return boolean
function sage.input.SourcePressed(source) end
---@param source string
---@return boolean
function sage.input.SourceReleased(source) end
---@param source string
---@return number
function sage.input.SourceValue(source) end
---@param name string
---@return boolean
function sage.input.Triggered(name) end
---@return string
function sage.input.TypedText() end
---@param name string
---@return Vec2
function sage.input.Vector(name) end
---@param name string
---@return boolean
function sage.input.WasPressed(name) end
---@param name string
---@return boolean
function sage.input.WasReleased(name) end

---@class sage.lensflare
sage.lensflare = {}

---Короткое имя того же модуля: `lensflare.X()` — это ТОТ ЖЕ объект,
---что `sage.lensflare`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.lensflare
lensflare = nil

---@return boolean
function sage.lensflare.Enabled() end
---@param t table
function sage.lensflare.Set(t) end

---@class sage.light
sage.light = {}

---Короткое имя того же модуля: `light.X()` — это ТОТ ЖЕ объект,
---что `sage.light`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.light
light = nil

---@return any
function sage.light.Get() end
---@param name string
---@return boolean
function sage.light.SetSkyPreset(name) end
---@param t table
function sage.light.SetSun(t) end

---@class sage.material
sage.material = {}

---Короткое имя того же модуля: `material.X()` — это ТОТ ЖЕ объект,
---что `sage.material`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.material
material = nil

---@param name string
---@param props? table
---@return string
function sage.material.Create(name, props) end
---@param name string
---@param path string
---@return boolean
function sage.material.Save(name, path) end
---@param name string
---@param propsOrKey any
---@param value? any
function sage.material.Set(name, propsOrKey, value) end

---@class sage.math
sage.math = {}

---Короткое имя того же модуля: `math.X()` — это ТОТ ЖЕ объект,
---что `sage.math`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.math
math = nil

---@param x number
---@param lo number
---@param hi number
---@return number
function sage.math.Clamp(x, lo, hi) end
---@param a Vec3
---@param b Vec3
---@return Vec3
function sage.math.Cross(a, b) end
---@param rad number
---@return number
function sage.math.Degrees(rad) end
---@param x number
---@return number
function sage.math.Fract(x) end
---@param x integer
---@param y? integer
---@param z? integer
---@param seed? integer
---@return number
function sage.math.Hash(x, y, z, seed) end
---@param origin Vec3
---@param dir Vec3
---@param planePoint Vec3
---@param planeNormal Vec3
---@return Vec3
function sage.math.IntersectRayPlane(origin, dir, planePoint, planeNormal) end
---@param origin Vec3
---@param dir Vec3
---@param center Vec3
---@param radius number
---@return Vec3
function sage.math.IntersectRaySphere(origin, dir, center, radius) end
---@param a number
---@param b number
---@param v number
---@return number
function sage.math.InverseLerp(a, b, v) end
---@param t number
---@param len number
---@return number
function sage.math.PingPong(t, len) end
---@param deg number
---@return number
function sage.math.Radians(deg) end
---@return Vec2
function sage.math.RandomInsideUnitCircle() end
---@return Vec3
function sage.math.RandomInsideUnitSphere() end
---@return Vec3
function sage.math.RandomOnUnitSphere() end
---@param lo number
---@param hi number
---@return number
function sage.math.RandomRange(lo, hi) end
---@param v number
---@param a0 number
---@param a1 number
---@param b0 number
---@param b1 number
---@return number
function sage.math.Remap(v, a0, a1, b0, b1) end
---@param seed? number
---@return any
function sage.math.Rng(seed) end
---@param a Vec3
---@param b Vec3
---@param t number
---@return Vec3
function sage.math.Slerp(a, b, t) end
---@param edge0 number
---@param edge1 number
---@param x number
---@return number
function sage.math.SmoothStep(edge0, edge1, x) end
---@param e0 number
---@param e1 number
---@param x number
---@return number
function sage.math.SmootherStep(e0, e1, x) end
---@param edge number
---@param x number
---@return number
function sage.math.Step(edge, x) end
---@param x number
---@param lo number
---@param hi number
---@return number
function sage.math.Wrap(x, lo, hi) end

---@class sage.mesh
sage.mesh = {}

---Короткое имя того же модуля: `mesh.X()` — это ТОТ ЖЕ объект,
---что `sage.mesh`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.mesh
mesh = nil

---@param size any
---@return any
function sage.mesh.Box(size) end
---@param r number
---@param h number
---@param seg? integer
---@param caps? boolean
---@return any
function sage.mesh.Cylinder(r, h, seg, caps) end
---@param source any
---@param opts? table
---@return any
function sage.mesh.Extrude(source, opts) end
---@param sx number
---@param sz number
---@param rx integer
---@param rz integer
---@param h? function
---@return any
function sage.mesh.Grid(sx, sz, rx, rz, h) end
---@return any
function sage.mesh.New() end
---@param r number
---@param seg? integer
---@param rings? integer
---@return any
function sage.mesh.Sphere(r, seg, rings) end

---@class sage.msg
sage.msg = {}

---Короткое имя того же модуля: `msg.X()` — это ТОТ ЖЕ объект,
---что `sage.msg`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.msg
msg = nil

---@param name string
---@param data any
function sage.msg.Broadcast(name, data) end
---@param target any
---@param name string
---@param ...? ...
---@return any
function sage.msg.Call(target, name, ...) end
---@param target any
---@param name string
---@param data any
function sage.msg.Send(target, name, data) end

---@class sage.noise
sage.noise = {}

---Короткое имя того же модуля: `noise.X()` — это ТОТ ЖЕ объект,
---что `sage.noise`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.noise
noise = nil

---@param opts? table
---@return any
function sage.noise.New(opts) end
---@param x number
---@param y number
---@param z? number
---@return number
function sage.noise.Perlin(x, y, z) end
---@param seed integer
function sage.noise.Seed(seed) end
---@param x number
---@param y number
---@param z? number
---@return number
function sage.noise.Simplex(x, y, z) end
---@param x number
---@param y number
---@param z? number
---@return number
function sage.noise.Value(x, y, z) end
---@param p Vec3
---@param strength number
---@param opts? table
---@return Vec3
function sage.noise.Warp(p, strength, opts) end
---@param x number
---@param y number
---@param z? number
---@return number
---@return number
---@return number
function sage.noise.Worley(x, y, z) end

---@class sage.physics
sage.physics = {}

---Короткое имя того же модуля: `physics.X()` — это ТОТ ЖЕ объект,
---что `sage.physics`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.physics
physics = nil

---@param entity Entity
---@param impulse Vec3
function sage.physics.AddAngularImpulse(entity, impulse) end
---@param entity Entity
---@param radius? number
---@param height? number
function sage.physics.AddCharacter(entity, radius, height) end
---@param entity Entity
---@param force Vec3
---@param point? Vec3
function sage.physics.AddForce(entity, force, point) end
---@param entity Entity
---@param impulse Vec3
function sage.physics.AddImpulse(entity, impulse) end
---@param entity Entity
---@param impulse Vec3
---@param point Vec3
function sage.physics.AddImpulseAtPoint(entity, impulse, point) end
---@param entity Entity
---@param torque Vec3
function sage.physics.AddTorque(entity, torque) end
---@param entity Entity
---@param feet Vec3
---@return boolean
function sage.physics.CharacterFits(entity, feet) end
---@param entity Entity
---@return any
function sage.physics.CharacterState(entity) end
---@return table
function sage.physics.Collisions() end
---@param entity Entity
---@return Vec3
function sage.physics.GetAngularVelocity(entity) end
---@param entity Entity
---@return Vec3
function sage.physics.GetVelocity(entity) end
---@param entity Entity
---@return Vec3
function sage.physics.GroundNormal(entity) end
---@param entity Entity
---@return boolean
function sage.physics.IsGrounded(entity) end
---@param zone Entity
---@param entity Entity
---@return boolean
function sage.physics.IsInTrigger(zone, entity) end
---@param entity Entity
---@return boolean
function sage.physics.IsSleeping(entity) end
---@param entity Entity
---@param velocity Vec3
---@param dt number
function sage.physics.MoveCharacter(entity, velocity, dt) end
---@param zone Entity
---@return table
function sage.physics.ObjectsInTrigger(zone) end
---@param center Vec3
---@param radius number
---@param mask? integer
---@return table
function sage.physics.OverlapSphere(center, radius, mask) end
---@param origin Vec3
---@param dir Vec3
---@param maxDistance number
---@param mask? integer
---@return any
function sage.physics.Raycast(origin, dir, maxDistance, mask) end
---@param entity Entity
---@param w Vec3
function sage.physics.SetAngularVelocity(entity, w) end
---@param entity Entity
---@param pos Vec3
function sage.physics.SetCharacterPosition(entity, pos) end
---@param entity Entity
---@param t table
function sage.physics.SetCharacterShape(entity, t) end
---@param entity Entity
---@param fn any
function sage.physics.SetCharacterWorld(entity, fn) end
---@param g Vec3
function sage.physics.SetGravity(g) end
---@param entity Entity
---@param layer integer
function sage.physics.SetLayer(entity, layer) end
---@param entity Entity
---@param on boolean
function sage.physics.SetSensor(entity, on) end
---@param entity Entity
---@param mask integer
function sage.physics.SetTriggerMask(entity, mask) end
---@param entity Entity
---@param v Vec3
function sage.physics.SetVelocity(entity, v) end
---@param entity Entity
---@return table
function sage.physics.TriggersOf(entity) end

---@class sage.post
sage.post = {}

---Короткое имя того же модуля: `post.X()` — это ТОТ ЖЕ объект,
---что `sage.post`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.post
post = nil

---@param kind string
---@return boolean
function sage.post.add(kind) end
---@param kind string
---@param on boolean
function sage.post.enable(kind, on) end
---@param kind string
---@param param string
---@return number
function sage.post.get(kind, param) end
---@param kind string
---@return boolean
function sage.post.has(kind) end
---@return table
function sage.post.kinds() end
---@param entity Entity
---@param kind string
---@return boolean
function sage.post.object_add(entity, kind) end
---@param entity Entity
function sage.post.object_clear(entity) end
---@param entity Entity
---@param kind string
---@param on boolean
function sage.post.object_enable(entity, kind, on) end
---@param entity Entity
---@param kind string
---@param param string
---@return number
function sage.post.object_get(entity, kind, param) end
---@param entity Entity
---@param kind string
---@return boolean
function sage.post.object_remove(entity, kind) end
---@param entity Entity
---@param kind string
---@param params table
function sage.post.object_set(entity, kind, params) end
---@param kind string
---@return boolean
function sage.post.remove(kind) end
---@param kind string
---@param params table
function sage.post.set(kind, params) end

---@class sage.reflect
sage.reflect = {}

---Короткое имя того же модуля: `reflect.X()` — это ТОТ ЖЕ объект,
---что `sage.reflect`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.reflect
reflect = nil

function sage.reflect.DisablePlanar() end
---@param on boolean
function sage.reflect.SetEnabled(on) end
---@param v number
function sage.reflect.SetIntensity(v) end
---@param normal Vec3
---@param point Vec3
function sage.reflect.SetPlanar(normal, point) end
---@param scale number
function sage.reflect.SetPlanarScale(scale) end
---@param height number
function sage.reflect.SetWater(height) end

---@class sage.render
sage.render = {}

---Короткое имя того же модуля: `render.X()` — это ТОТ ЖЕ объект,
---что `sage.render`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.render
render = nil

---@param mat any
function sage.render.ClearMaterialShader(mat) end
---@param entity Entity
function sage.render.ClearShaderParams(entity) end
---@return table
function sage.render.DebugViews() end
---@return string
function sage.render.GetDebugView() end
---@param path string
---@return any
function sage.render.GetMaterial(path) end
---@param path string
---@param name string
---@return any
function sage.render.GetMaterialParam(path, name) end
---@param entity Entity
---@return any
function sage.render.MaterialOf(entity) end
---@param name string
---@return any
function sage.render.NewMaterial(name) end
---@param path string
---@return any
function sage.render.ReloadMaterial(path) end
---@param mat any
function sage.render.ResolveMaterialTextures(mat) end
---@param mat any
---@param path string
function sage.render.SaveMaterial(mat, path) end
---@param name string
---@return boolean
function sage.render.SetDebugView(name) end
---@param entity Entity
---@param path string
function sage.render.SetMaterial(entity, path) end
---@param path string
---@param name string
---@param value any
function sage.render.SetMaterialParam(path, name, value) end
---@param mat any
---@param vertexPath string
---@param fragmentPath string
function sage.render.SetMaterialShader(mat, vertexPath, fragmentPath) end
---@param entity Entity
function sage.render.SetMeshCapsule(entity) end
---@param entity Entity
function sage.render.SetMeshCone(entity) end
---@param entity Entity
function sage.render.SetMeshCube(entity) end
---@param entity Entity
function sage.render.SetMeshCylinder(entity) end
---@param entity Entity
---@param path string
function sage.render.SetMeshModel(entity, path) end
---@param entity Entity
function sage.render.SetMeshNone(entity) end
---@param entity Entity
function sage.render.SetMeshPlane(entity) end
---@param entity Entity
function sage.render.SetMeshSphere(entity) end
---@param entity Entity
---@param name string
---@param value any
function sage.render.SetShaderParam(entity, name, value) end
---@param entity Entity
---@param part integer
---@param path string
function sage.render.SetSubmeshMaterial(entity, part, path) end
---@param entity Entity
---@return integer
function sage.render.SubmeshCount(entity) end

---@class sage.rt
sage.rt = {}

---Короткое имя того же модуля: `rt.X()` — это ТОТ ЖЕ объект,
---что `sage.rt`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.rt
rt = nil

---@param entity Entity
---@param t table
function sage.rt.Attach(entity, t) end
---@param name string
---@return boolean
function sage.rt.Ready(name) end
---@param entity Entity
function sage.rt.Refresh(entity) end

---@class sage.save
sage.save = {}

---Короткое имя того же модуля: `save.X()` — это ТОТ ЖЕ объект,
---что `sage.save`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.save
save = nil

---@param slot string
---@return boolean
function sage.save.Delete(slot) end
---@return string
function sage.save.Directory() end
---@param slot string
---@return boolean
function sage.save.Exists(slot) end
---@param slot string
---@return boolean
function sage.save.HasBackup(slot) end
---@param slot string
---@return any
function sage.save.Info(slot) end
---@param slot string
---@return any
function sage.save.Read(slot) end
---@param slot string
---@return boolean
function sage.save.RestoreBackup(slot) end
---@return table
function sage.save.Slots() end
---@param slot string
---@return integer
function sage.save.Version(slot) end
---@param slot string
---@param data table
---@param options any
---@return boolean
function sage.save.Write(slot, data, options) end

---@class sage.scene
sage.scene = {}

---Короткое имя того же модуля: `scene.X()` — это ТОТ ЖЕ объект,
---что `sage.scene`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.scene
scene = nil

---@param target any
function sage.scene.Destroy(target) end
---@param name string
---@return Entity
function sage.scene.Find(name) end
---@param id integer
---@return Entity
function sage.scene.FindById(id) end
---@param name string
function sage.scene.Load(name) end
---@param id integer
---@param path string
---@return boolean
function sage.scene.SavePrefab(id, path) end
---@param name string
---@return Entity
function sage.scene.Spawn(name) end
---@param path string
---@param at? Vec3
---@return integer
function sage.scene.SpawnPrefab(path, at) end
---@param pos Vec3
---@param scale? number
---@return integer
function sage.scene.SpawnRagdoll(pos, scale) end

---@class sage.texture
sage.texture = {}

---Короткое имя того же модуля: `texture.X()` — это ТОТ ЖЕ объект,
---что `sage.texture`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.texture
texture = nil

---@param name string
---@param t table
---@return string
function sage.texture.Generate(name, t) end
---@param path string
---@param name? string
---@return any
function sage.texture.Load(path, name) end
---@param name string
---@param w integer
---@param h integer
---@return any
function sage.texture.New(name, w, h) end

---@class sage.time
sage.time = {}

---Короткое имя того же модуля: `time.X()` — это ТОТ ЖЕ объект,
---что `sage.time`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.time
time = nil

---@param id integer
function sage.time.Cancel(id) end
---@param intervalSeconds number
---@param fn function
---@return integer
function sage.time.Repeat(intervalSeconds, fn) end
---@return number
function sage.time.Scale() end
---@param seconds number
---@param fn function
---@return integer
function sage.time.Schedule(seconds, fn) end
---@param scale number
function sage.time.SetScale(scale) end
---@param fn function
function sage.time.StartCoroutine(fn) end
---@param unixSeconds? number
---@return table
function sage.time.local_date(unixSeconds) end
---@return number
function sage.time.unix_now() end

---@class sage.tween
sage.tween = {}

---Короткое имя того же модуля: `tween.X()` — это ТОТ ЖЕ объект,
---что `sage.tween`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.tween
tween = nil

---@return integer
function sage.tween.Active() end
---@param id integer
function sage.tween.Cancel(id) end
function sage.tween.CancelAll() end
---@param entity Entity
---@param to Vec3
---@param dur number
---@param ease any
---@return integer
function sage.tween.Color(entity, to, dur, ease) end
---@param entity Entity
---@param to Vec3
---@param dur number
---@param ease any
---@return integer
function sage.tween.Move(entity, to, dur, ease) end
---@param entity Entity
---@param toEulerDeg Vec3
---@param dur number
---@param ease any
---@return integer
function sage.tween.Rotate(entity, toEulerDeg, dur, ease) end
---@param entity Entity
---@param to Vec3
---@param dur number
---@param ease any
---@return integer
function sage.tween.Scale(entity, to, dur, ease) end
---@param entity Entity
---@param to number
---@param dur number
---@param ease any
---@return integer
function sage.tween.UIValue(entity, to, dur, ease) end

---@class sage.ui
sage.ui = {}

---Короткое имя того же модуля: `ui.X()` — это ТОТ ЖЕ объект,
---что `sage.ui`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.ui
ui = nil

---@param entity Entity
function sage.ui.ClearImage(entity) end
---@param entity Entity
function sage.ui.ClearLayout(entity) end
---@return string
function sage.ui.ClickedAction() end
---@param entity Entity
---@param signal string
---@param fn function
---@return integer
function sage.ui.Connect(entity, signal, fn) end
---@return Vec2
function sage.ui.Cursor() end
---@param x number
---@param y number
---@param screenW integer
---@param screenH integer
---@return Entity
function sage.ui.ElementAt(x, y, screenW, screenH) end
---@param entity Entity
---@return number
function sage.ui.GetValue(entity) end
---@param name string
---@return boolean
function sage.ui.HasIcon(name) end
---@return string
function sage.ui.HoveredAction() end
---@return table
function sage.ui.IconNames() end
---@param path string
---@return integer
---@return integer
function sage.ui.ImageSize(path) end
---@param entity Entity
---@return boolean
function sage.ui.LoadSlice(entity) end
---@return boolean
function sage.ui.MouseDown() end
---@param entity Entity
---@param preset string
function sage.ui.Preset(entity, preset) end
---@return string
function sage.ui.PressedAction() end
---@return string
function sage.ui.ReleasedAction() end
---@return Vec2
function sage.ui.ScreenSize() end
---@param entity Entity
---@param opts table
function sage.ui.SetCanvas(entity, opts) end
---@param entity Entity
---@param path string
---@param sharp? boolean
function sage.ui.SetImage(entity, path, sharp) end
---@param entity Entity
---@param opts table
function sage.ui.SetLayout(entity, opts) end
---@param entity Entity
---@param part string
---@param value any
function sage.ui.SetLook(entity, part, value) end
---@param entity Entity
---@param value any
function sage.ui.SetMask(entity, value) end
---@param entity Entity
---@param v any
function sage.ui.SetPressedOffset(entity, v) end
---@param entity Entity
---@param step number
function sage.ui.SetRangeStep(entity, step) end
---@param entity Entity
---@param l number
---@param t number
---@param r number
---@param b number
---@param scale? number
function sage.ui.SetSlice(entity, l, t, r, b, scale) end
---@param entity Entity
---@param draw boolean
function sage.ui.SetSliceDrawsCenter(entity, draw) end
---@param entity Entity
---@param tiled boolean
function sage.ui.SetSliceTiled(entity, tiled) end
---@param entity Entity
---@param x number
---@param y number
---@param w number
---@param h number
function sage.ui.SetSprite(entity, x, y, w, h) end
---@param entity Entity
---@param path string
---@param name string
---@param sharp? boolean
---@return boolean
function sage.ui.SetSpriteNamed(entity, path, name, sharp) end
---@param entity Entity
---@param t table
function sage.ui.SetTextColors(entity, t) end
---@param entity Entity
---@param value number
function sage.ui.SetValue(entity, value) end
---@param path string
---@return any
function sage.ui.Sheet(path) end
---@param name string
---@return integer
function sage.ui.SpawnDemo(name) end
---@return integer
function sage.ui.SpawnShowcase() end

---@class sage.user_folder
sage.user_folder = {}

---Короткое имя того же модуля: `user_folder.X()` — это ТОТ ЖЕ объект,
---что `sage.user_folder`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.user_folder
user_folder = nil

---@param name string
---@return boolean
function sage.user_folder.changed(name) end
---@return table
function sage.user_folder.folders() end
---@param name string
---@param recursive? boolean
---@return table
function sage.user_folder.list(name, recursive) end
---@param name string
---@return boolean
function sage.user_folder.open(name) end
---@param name string
---@return string
function sage.user_folder.path(name) end

---@class sage.visual
sage.visual = {}

---Короткое имя того же модуля: `visual.X()` — это ТОТ ЖЕ объект,
---что `sage.visual`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.visual
visual = nil

---@param name string
---@param inputs? table
---@return any
function sage.visual.call(name, inputs) end
---@return table
function sage.visual.functions() end
---@param path string
---@return string
function sage.visual.graph(path) end
---@param id string
---@param spec table
function sage.visual.node(id, spec) end
---@param entity any
---@return any
function sage.visual.of(entity) end
---@param id string
---@param spec table
function sage.visual.type(id, spec) end

---@class sage.volumetric
sage.volumetric = {}

---Короткое имя того же модуля: `volumetric.X()` — это ТОТ ЖЕ объект,
---что `sage.volumetric`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.volumetric
volumetric = nil

---@return boolean
function sage.volumetric.Enabled() end
---@param t table
function sage.volumetric.Set(t) end

-- --- Старые глобальные имена ----------------------------------------------
-- Те же функции, что выше, под именами, с которых начинался движок.
-- Работают и работать будут: на них написаны существующие игры.
ActiveTweens = sage.tween.Active
AddAngularImpulse = sage.physics.AddAngularImpulse
AddAnimatedModel = sage.anim.Add
AddBillboard = sage.fx.AddBillboard
AddCharacterController = sage.physics.AddCharacter
AddForce = sage.physics.AddForce
AddIKGoal = sage.ik.AddGoal
AddImpulse = sage.physics.AddImpulse
AddImpulseAtPoint = sage.physics.AddImpulseAtPoint
AddRenderTexture = sage.rt.Attach
AddTorque = sage.physics.AddTorque
AnimationCount = sage.anim.Count
AnimationDuration = sage.anim.Duration
AnimationFade = sage.anim.Fade
AnimationIndex = sage.anim.Index
AnimationNames = sage.anim.Names
AnimationTime = sage.anim.Time
ApplySkyPreset = sage.light.SetSkyPreset
BindAction = sage.input.Bind
BindActionIn = sage.input.BindIn
BindAxis = sage.input.BindAxis
BindVector = sage.input.BindVector
BoneAxis = sage.anim.BoneAxis
BonePosition = sage.anim.BonePosition
BorrowAnimations = sage.anim.Borrow
Broadcast = sage.msg.Broadcast
CallScript = sage.msg.Call
CancelTimer = sage.time.Cancel
CharacterFits = sage.physics.CharacterFits
CharacterState = sage.physics.CharacterState
Clamp = sage.math.Clamp
ClearJointPoses = sage.anim.ClearJointPoses
ClearMaterialShader = sage.render.ClearMaterialShader
ClearShaderParams = sage.render.ClearShaderParams
ClearUIImage = sage.ui.ClearImage
ClearUILayout = sage.ui.ClearLayout
Collisions = sage.physics.Collisions
CreateInputContext = sage.input.CreateContext
CreateParticleStream = sage.fx.CreateStream
Cross = sage.math.Cross
DebugViews = sage.render.DebugViews
Degrees = sage.math.Degrees
DeleteSave = sage.save.Delete
DestroyObject = sage.scene.Destroy
DisablePlanarReflection = sage.reflect.DisablePlanar
EmitEvent = sage.events.Emit
EmitFromObject = sage.fx.EmitFrom
EmitParticles = sage.fx.Emit
EventCount = sage.events.Count
FadeOutMusic = sage.audio.FadeOutMusic
FindObject = sage.scene.Find
FindObjectById = sage.scene.FindById
GenerateTexture = sage.texture.Generate
GetAngularVelocity = sage.physics.GetAngularVelocity
GetAxis = sage.input.Axis
GetCamera = sage.camera.Get
GetCameraPasses = sage.camera.passes
GetDebugView = sage.render.GetDebugView
GetLighting = sage.light.Get
GetMaterial = sage.render.GetMaterial
GetMaterialParam = sage.render.GetMaterialParam
GetMouseDelta = sage.input.MouseDelta
GetMousePosition = sage.input.MousePosition
GetScrollDelta = sage.input.ScrollDelta
GetUIElementAt = sage.ui.ElementAt
GetUIValue = sage.ui.GetValue
GetVector = sage.input.Vector
GetVelocity = sage.physics.GetVelocity
GroundNormal = sage.physics.GroundNormal
HasAction = sage.input.Has
HasIcon = sage.ui.HasIcon
HasSave = sage.save.Exists
HasSaveBackup = sage.save.HasBackup
IKLocked = sage.ik.Locked
IconNames = sage.ui.IconNames
ImageSize = sage.ui.ImageSize
IntersectRayPlane = sage.math.IntersectRayPlane
IntersectRaySphere = sage.math.IntersectRaySphere
IsActionDown = sage.input.IsDown
IsGrounded = sage.physics.IsGrounded
IsInTrigger = sage.physics.IsInTrigger
IsInputContextEnabled = sage.input.IsContextEnabled
IsMouseCaptured = sage.input.IsMouseCaptured
IsMusicPlaying = sage.audio.IsMusicPlaying
IsPlaying = sage.audio.IsPlaying
IsSleeping = sage.physics.IsSleeping
JointIndex = sage.anim.JointIndex
JointNames = sage.anim.JointNames
LaunchArg = sage.app.Arg
LaunchFlag = sage.app.Flag
LensFlareEnabled = sage.lensflare.Enabled
LoadGame = sage.save.Read
LoadInputMapping = sage.input.LoadMapping
LoadParticleEffect = sage.fx.LoadEffect
LoadScene = sage.scene.Load
LoadUISlice = sage.ui.LoadSlice
MaterialOf = sage.render.MaterialOf
MoveCharacter = sage.physics.MoveCharacter
NewMaterial = sage.render.NewMaterial
ObjectsInTrigger = sage.physics.ObjectsInTrigger
OffEvent = sage.events.Off
OnEvent = sage.events.On
OnceEvent = sage.events.Once
OverlapSphere = sage.physics.OverlapSphere
Play = sage.audio.Play
PlayAnimation = sage.anim.Play
PlayAnimationNamed = sage.anim.PlayNamed
PlayMusic = sage.audio.PlayMusic
PlayParticles = sage.fx.Play
PlaySound = sage.audio.PlaySound
PlaySound3D = sage.audio.PlaySound3D
Radians = sage.math.Radians
RandomInsideUnitCircle = sage.math.RandomInsideUnitCircle
RandomInsideUnitSphere = sage.math.RandomInsideUnitSphere
RandomOnUnitSphere = sage.math.RandomOnUnitSphere
RandomRange = sage.math.RandomRange
Raycast = sage.physics.Raycast
RebindAction = sage.input.Rebind
RefreshRenderTexture = sage.rt.Refresh
ReloadMaterial = sage.render.ReloadMaterial
RemoveBillboard = sage.fx.RemoveBillboard
RemoveParticleStream = sage.fx.RemoveStream
RenderTextureReady = sage.rt.Ready
Repeat = sage.time.Repeat
ResolveMaterialTextures = sage.render.ResolveMaterialTextures
RestoreSaveBackup = sage.save.RestoreBackup
SaveDirectory = sage.save.Directory
SaveGame = sage.save.Write
SaveInfo = sage.save.Info
SaveInputMapping = sage.input.SaveMapping
SaveMaterial = sage.render.SaveMaterial
SavePrefab = sage.scene.SavePrefab
SaveSlots = sage.save.Slots
SaveVersion = sage.save.Version
Schedule = sage.time.Schedule
SeekAnimation = sage.anim.Seek
SendMessage = sage.msg.Send
SetActionTrigger = sage.input.SetTrigger
SetAngularVelocity = sage.physics.SetAngularVelocity
SetAnimationLoop = sage.anim.SetLoop
SetAnimationPlaying = sage.anim.SetPlaying
SetAnimationSpeed = sage.anim.SetSpeed
SetBillboardPosition = sage.fx.SetBillboardPosition
SetBillboardTint = sage.fx.SetBillboardTint
SetBillboardVisible = sage.fx.SetBillboardVisible
SetCameraPasses = sage.camera.set_passes
SetCharacterPosition = sage.physics.SetCharacterPosition
SetCharacterShape = sage.physics.SetCharacterShape
SetCharacterWorld = sage.physics.SetCharacterWorld
SetClip = sage.audio.SetClip
SetDebugView = sage.render.SetDebugView
SetGravity = sage.physics.SetGravity
SetIKAim = sage.ik.SetAim
SetIKAlign = sage.ik.SetAlign
SetIKEnabled = sage.ik.SetEnabled
SetIKFootLock = sage.ik.SetFootLock
SetIKPole = sage.ik.SetPole
SetIKTarget = sage.ik.SetTarget
SetIKWeight = sage.ik.SetWeight
SetInputContextEnabled = sage.input.SetContextEnabled
SetJointOffset = sage.anim.SetJointOffset
SetJointRotation = sage.anim.SetJointRotation
SetLensFlare = sage.lensflare.Set
SetMasterVolume = sage.audio.SetMasterVolume
SetMaterial = sage.render.SetMaterial
SetMaterialParam = sage.render.SetMaterialParam
SetMaterialShader = sage.render.SetMaterialShader
SetMeshCapsule = sage.render.SetMeshCapsule
SetMeshCone = sage.render.SetMeshCone
SetMeshCube = sage.render.SetMeshCube
SetMeshCylinder = sage.render.SetMeshCylinder
SetMeshModel = sage.render.SetMeshModel
SetMeshNone = sage.render.SetMeshNone
SetMeshPlane = sage.render.SetMeshPlane
SetMeshSphere = sage.render.SetMeshSphere
SetMouseCaptured = sage.input.SetMouseCaptured
SetMusicVolume = sage.audio.SetMusicVolume
SetParticleStreamActive = sage.fx.SetStreamActive
SetParticleStreamPosition = sage.fx.SetStreamPosition
SetPauseMenu = sage.game.SetPauseMenu
SetPhysicsLayer = sage.physics.SetLayer
SetPitch = sage.audio.SetPitch
SetPlanarReflection = sage.reflect.SetPlanar
SetPlanarReflectionScale = sage.reflect.SetPlanarScale
SetReflectionIntensity = sage.reflect.SetIntensity
SetReflectionsEnabled = sage.reflect.SetEnabled
SetRootMotion = sage.anim.SetRootMotion
SetSensor = sage.physics.SetSensor
SetShaderParam = sage.render.SetShaderParam
SetSubmeshMaterial = sage.render.SetSubmeshMaterial
SetSun = sage.light.SetSun
SetTriggerMask = sage.physics.SetTriggerMask
SetUICanvas = sage.ui.SetCanvas
SetUIImage = sage.ui.SetImage
SetUILayout = sage.ui.SetLayout
SetUIPreset = sage.ui.Preset
SetUISlice = sage.ui.SetSlice
SetUISliceDrawsCenter = sage.ui.SetSliceDrawsCenter
SetUISliceTiled = sage.ui.SetSliceTiled
SetUISprite = sage.ui.SetSprite
SetUISpriteNamed = sage.ui.SetSpriteNamed
SetUIValue = sage.ui.SetValue
SetVelocity = sage.physics.SetVelocity
SetVolume = sage.audio.SetVolume
SetVolumetrics = sage.volumetric.Set
SetWaterReflection = sage.reflect.SetWater
Slerp = sage.math.Slerp
SmoothStep = sage.math.SmoothStep
SpawnObject = sage.scene.Spawn
SpawnPrefab = sage.scene.SpawnPrefab
SpawnRagdoll = sage.scene.SpawnRagdoll
SpawnUIDemo = sage.ui.SpawnDemo
SpawnUIShowcase = sage.ui.SpawnShowcase
StartCoroutine = sage.time.StartCoroutine
Stop = sage.audio.Stop
StopMusic = sage.audio.StopMusic
StopParticles = sage.fx.Stop
SubmeshCount = sage.render.SubmeshCount
TriggersOf = sage.physics.TriggersOf
TweenCancel = sage.tween.Cancel
TweenCancelAll = sage.tween.CancelAll
TweenColor = sage.tween.Color
TweenMove = sage.tween.Move
TweenRotate = sage.tween.Rotate
TweenScale = sage.tween.Scale
TweenUIValue = sage.tween.UIValue
UIClickedAction = sage.ui.ClickedAction
UICursor = sage.ui.Cursor
UIHoveredAction = sage.ui.HoveredAction
UIMouseDown = sage.ui.MouseDown
UIPressedAction = sage.ui.PressedAction
UIReleasedAction = sage.ui.ReleasedAction
UIScreenSize = sage.ui.ScreenSize
VolumetricsEnabled = sage.volumetric.Enabled
WasActionPressed = sage.input.WasPressed
WasActionReleased = sage.input.WasReleased
WasActionTriggered = sage.input.Triggered
log = sage.core.log
logError = sage.core.logError
logWarn = sage.core.logWarn
-- --- Процедурная генерация в стиле SAGE (ScriptApi_ProcGen.cpp) ----------
---Цвет из трёх долей 0..1.
---@param r number
---@param g number
---@param b number
---@return Vec3
function rgb(r, g, b) end

---Серый цвет яркости v (0..1).
---@param v number
---@return Vec3
function gray(v) end

---Шум Перлина в [-1, 1]. z можно не давать.
---@param x number
---@param y? number
---@param z? number
---@return number
function perlin(x, y, z) end

---Симплекс-шум в [-1, 1].
---@param x number
---@param y? number
---@param z? number
---@return number
function simplex(x, y, z) end

---Value-шум в [-1, 1]: мягче, «облачнее» градиентного.
---@param x number
---@param y? number
---@param z? number
---@return number
function value_noise(x, y, z) end

---Клеточный шум: расстояние до ближайшей точки, до второй и номер клетки.
---@param x number
---@param y? number
---@param z? number
---@return number f1
---@return number f2
---@return number cell
function worley(x, y, z) end

---Фрактальный шум (несколько слоёв Перлина) в [-1, 1].
---@param x number
---@param y? number
---@param z? number
---@return number
function fbm(x, y, z) end

---Гребни: горные хребты, трещины. В [0, 1].
---@param x number
---@param y? number
---@param z? number
---@return number
function ridged(x, y, z) end

---Турбулентность: |шум| по слоям — дым, мрамор. В [0, 1].
---@param x number
---@param y? number
---@param z? number
---@return number
function turbulence(x, y, z) end

---Точка, сдвинутая шумом: искажение для «живых» узоров.
---@param x number
---@param y number
---@param z number
---@param strength? number
---@return Vec3
function warp(x, y, z, strength) end

---Зерно для perlin, simplex, fbm и прочих простых функций шума.
---@param seed integer
function set_seed(seed) end

---Свой генератор шума: noise{kind = "simplex", seed = 42, octaves = 5, scale = 0.02}.
---@class sage.noise
---@overload fun(opts?: table|string): Noise
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
---@field grid fun(x:number, y:number, z:number, nx:integer, ny:integer, nz:integer, step?:number, mode?:string): number[]
---@overload fun(x:number, y?:number, z?:number): number
---Число из диапазона [a, b] — в диапазон [c, d].
---@return number
function remap(v, a, b, c, d) end

---Где v между a и b: 0 в a, 1 в b.
---@return number
function inverse_lerp(a, b, v) end

---Между a и b по доле t.
---@return number
function lerp(a, b, t) end

---x в границах [lo, hi].
---@return number
function clamp(x, lo, hi) end

---Дробная часть.
---@return number
function fract(x) end

---x, замкнутый по кругу в [lo, hi).
---@return number
function wrap(x, lo, hi) end

---Туда-обратно в [0, length].
---@return number
function ping_pong(t, length) end

---0 до порога, 1 после.
---@return number
function step(edge, x) end

---Плавная ступенька между e0 и e1.
---@return number
function smoothstep(e0, e1, x) end

---Ещё более плавная ступенька.
---@return number
function smootherstep(e0, e1, x) end

---Детерминированное «случайное» число 0..1 из целых координат.
---@return number
function hash(x, y, z, seed) end

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
function random(seed) end

---Формула из строки — обычная функция: formula("sin(x) * y", "x, y").
---Внутри видны math, шумы и помощники: sin, clamp, fbm, lerp...
---@param expr string
---@param args? string
---@return function
function formula(expr, args) end

---@class Mesh
---@field vertices integer
---@field triangles integer
---@field vertex fun(x:number|Vec3, y?:number|Vec3, z?:number|Vec2, normal?:Vec3, uv?:Vec2, uv2?:Vec2): integer
---@field arrays fun(data:{position:number[], normal?:number[], uv?:number[], uv2?:number[], index:integer[]}): integer
---@field triangle fun(a:integer, b:integer, c:integer)
---@field quad fun(a:integer, b:integer, c:integer, d:integer)
---@field material fun(slot:integer)
---@field position fun(i:integer): Vec3
---@field set_position fun(i:integer, x:number|Vec3, y?:number, z?:number)
---@field set_normal fun(i:integer, x:number|Vec3, y?:number, z?:number)
---@field set_uv fun(i:integer, u:number, v:number)
---@field set_uv2 fun(i:integer, u:number, v:number)
---@field normals fun(smooth?:boolean)
---@field deform fun(fn:fun(p:Vec3, n:Vec3, uv:Vec2): Vec3)
---@field move fun(x:number|Vec3, y?:number, z?:number)
---@field scale fun(x:number|Vec3, y?:number, z?:number)
---@field add fun(other:Mesh)
---@field clear fun()
---@field bounds fun(): Vec3, Vec3
---@field apply fun(object:any)
---@field save fun(path:string)
---Пустой меш: вершины, треугольники, материалы — кодом.
---@class sage.mesh
---@overload fun(): Mesh
---Плоскость width x depth из cols x rows клеток; height(x, z) — высота точки.
---@param width number
---@param depth? number
---@param cols? integer
---@param rows? integer
---@param height? fun(x:number, z:number): number
---@return Mesh
function grid(width, depth, cols, rows, height) end

---Объёмный спрайт: кусок картинки, выдавленный в толщину по альфе (меч,
---ключ, листок). source — холст texture(...)/texture.load или путь;
---opts = {x, y, w, h, depth = 1, cutoff = 0.5}, кусок — от левого верхнего угла.
---Меш: длинная сторона = 1, центр в нуле, лицом к +Z. Материал — с вырезом по альфе.
---@param source Texture|string
---@param opts? table
---@return Mesh
function extrude(source, opts) end

---Коробка: одно число — куб, три — размеры по осям.
---@param w number
---@param h? number
---@param d? number
---@return Mesh
function box(w, h, d) end

---Сфера радиуса r.
---@param r number
---@param segments? integer
---@param rings? integer
---@return Mesh
function sphere(r, segments, rings) end

---Цилиндр радиуса r и высоты h; caps = false — без крышек.
---@param r number
---@param h number
---@param segments? integer
---@param caps? boolean
---@return Mesh
function cylinder(r, h, segments, caps) end

---@class Texture
---@field name string
---@field width integer
---@field height integer
---@field nearest boolean
---@field fill fun(fn:fun(u:number, v:number, x:integer, y:integer): Vec3|number)
---@field set fun(x:integer, y:integer, color:Vec3|number)
---@field get fun(x:integer, y:integer): Vec4
---@field apply fun()
---Текстура, нарисованная кодом: texture("noise", 256, 256).
---@class sage.texture
---@overload fun(name: string, width: integer, height?: integer): Texture
---Картинка с диска, пиксели которой читаются и правятся: texture.load("assets/a.png").
---Строки снизу вверх, как у texture(...). name — имя для материала (по умолчанию путь).
---@param path string
---@param name? string
---@return Texture
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
---Материал из кода: material("stone", {color = rgb(0.5, 0.45, 0.4), roughness = 0.9}).
---@class sage.material
---@overload fun(name: string, props?: table): Material
-- --- Интерфейс в стиле SAGE (ScriptApi_UIKit.cpp) ----------
---Экран: элемент во весь экран, корень интерфейса. order — порядок между
---экранами (больше — поверх), input = false — показывается, но не ловит мышь.
---scale = "pixel" — целый масштаб под опорный размер reference {w, h} (для
---пиксельной графики: на любом разрешении пиксели одинаковые), "fit" —
---плавный; max — предел целого масштаба. Без scale — единица = пиксель.
---@param o? table
---@return table
function ui.screen(o) end

---Панель: прямоугольник цветом или картинкой (девятина, плитка).
---@param o table
---@return table
function ui.panel(o) end

---Картинка: файл или кусок листа (sprite = "имя" из .sagesheet или {x, y, w, h}).
---@param o table
---@return table
function ui.image(o) end

---Надпись. size — высота строки в пикселях, align — left/center/right,
---valign — top/center/bottom; shadow_offset — тень (цвет вчетверо темнее).
---@param o table
---@return table
function ui.text(o) end

---Кнопка: вид (как у панели) и вид в состояниях hover/pressed/disabled —
---таблицами с теми же ключами; надпись — text и ключи надписи; цвет надписи в
---состояниях — hover_text_color, disabled_text_color. on_click = function(btn).
---@param o table
---@return table
function ui.button(o) end

---Поле ввода: text, max (длина), placeholder, password; вид — как у панели,
---focused — вид, пока в поле печатают.
---@param o table
---@return table
function ui.field(o) end

---Ползунок: value, min, max, step; вид частей — track, filled, knob
---(таблицы вида). on_change = function(slider, value).
---@param o table
---@return table
function ui.slider(o) end

---Полоса (здоровье, загрузка): value 0..1; вид фона — как у панели,
---заполнения — fill = {...}.
---@param o table
---@return table
function ui.bar(o) end

---Группа: невидимый контейнер. layout = "row" | "column" | "grid" расставляет
---детей сам: gap — зазор, padding — поля {л, в, п, н} или число, columns,
---justify = start | center | end | between | around | evenly, cross. У детей
---работает margin — свои поля вокруг. clip = true обрезает детей.
---@param o? table
---@return table
function ui.group(o) end

---Курсор мыши в пикселях интерфейса.
---@return Vec2
function ui.cursor() end

---Размер экрана в пикселях интерфейса.
---@return Vec2
function ui.size() end

---Зажата ли левая кнопка мыши.
---@return boolean
function ui.mouse_down() end


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

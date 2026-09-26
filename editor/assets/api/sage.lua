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
---@param other SageObject|nil  nil — гостя удалили, пока он был в зоне
function Script:OnTriggerExit(other) end
---@param other SageObject
function Script:OnTriggerStay(other) end
---@param name string
function Script:OnAnimationEvent(name) end

-- --- Модули ---------------------------------------------------------------
---@class sage
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
---@field math sage.math
---@field msg sage.msg
---@field physics sage.physics
---@field reflect sage.reflect
---@field render sage.render
---@field rt sage.rt
---@field save sage.save
---@field scene sage.scene
---@field texture sage.texture
---@field time sage.time
---@field tween sage.tween
---@field ui sage.ui
---@field volumetric sage.volumetric
sage = {}

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
---@return number
function sage.anim.Borrow(entity, path) end
---@param entity Entity
function sage.anim.ClearJointPoses(entity) end
---@param entity Entity
---@return number
function sage.anim.Count(entity) end
---@param entity Entity
---@param clip number
---@return number
function sage.anim.Duration(entity, clip) end
---@param entity Entity
---@return number
function sage.anim.Fade(entity) end
---@param entity Entity
---@param name string
---@return number
function sage.anim.Index(entity, name) end
---@param entity Entity
---@param name string
---@return number
function sage.anim.JointIndex(entity, name) end
---@param entity Entity
---@return table
function sage.anim.JointNames(entity) end
---@param entity Entity
---@return table
function sage.anim.Names(entity) end
---@param entity Entity
---@param clip number
---@param blend number
---@return boolean
function sage.anim.Play(entity, clip, blend) end
---@param entity Entity
---@param name string
---@param blend number
---@return boolean
function sage.anim.PlayNamed(entity, name, blend) end
---@param entity Entity
---@param time number
function sage.anim.Seek(entity, time) end
---@param entity Entity
---@param joint number
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

---@param entity Entity
function sage.audio.IsPlaying(entity) end
---@param entity Entity
function sage.audio.Play(entity) end
---@param path string
---@param volume number
function sage.audio.PlayMusic(path, volume) end
---@param path string
---@param volume number
function sage.audio.PlaySound(path, volume) end
---@param path string
---@param pos Vec3
---@param volume number
function sage.audio.PlaySound3D(path, pos, volume) end
---@param entity Entity
---@param clip string
function sage.audio.SetClip(entity, clip) end
---@param volume number
function sage.audio.SetMasterVolume(volume) end
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
---@return number
function sage.events.Count(name) end
---@param name string
---@param payload any
function sage.events.Emit(name, payload) end
---@param arg any
function sage.events.Off(arg) end
---@param name string
---@param fn function
---@return number
function sage.events.On(name, fn) end
---@param name string
---@param fn function
---@return number
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
---@param texturePath string
---@return number
function sage.fx.AddBillboard(pos, size, texturePath) end
---@param id string
---@param fx any
---@param pos Vec3
function sage.fx.CreateStream(id, fx, pos) end
---@param fx any
---@param pos Vec3
---@param count number
function sage.fx.Emit(fx, pos, count) end
---@param entity Entity
---@param count number
function sage.fx.EmitFrom(entity, count) end
---@param path string
function sage.fx.LoadEffect(path) end
---@param entity Entity
function sage.fx.Play(entity) end
---@param id number
function sage.fx.RemoveBillboard(id) end
---@param id string
function sage.fx.RemoveStream(id) end
---@param id number
---@param pos Vec3
function sage.fx.SetBillboardPosition(id, pos) end
---@param id number
---@param tint Vec4
function sage.fx.SetBillboardTint(id, tint) end
---@param id number
---@param visible boolean
function sage.fx.SetBillboardVisible(id, visible) end
---@param id string
---@param active boolean
function sage.fx.SetStreamActive(id, active) end
---@param id string
---@param pos Vec3
function sage.fx.SetStreamPosition(id, pos) end
---@param entity Entity
---@param clear boolean
function sage.fx.Stop(entity, clear) end

---@class sage.game
sage.game = {}

---Короткое имя того же модуля: `game.X()` — это ТОТ ЖЕ объект,
---что `sage.game`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.game
game = nil

function sage.game.HasPauseMenu() end
function sage.game.IsPaused() end
---@param paused boolean
function sage.game.Pause(paused) end
function sage.game.Quit() end
function sage.game.Restart() end
---@param enabled boolean
function sage.game.SetPauseMenu(enabled) end

---@class sage.ik
sage.ik = {}

---Короткое имя того же модуля: `ik.X()` — это ТОТ ЖЕ объект,
---что `sage.ik`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.ik
ik = nil

---@param entity Entity
---@param bone string
---@param chainLength number
---@return number
function sage.ik.AddGoal(entity, bone, chainLength) end
---@param entity Entity
---@param goal number
---@return boolean
function sage.ik.Locked(entity, goal) end
---@param entity Entity
---@param goal number
---@param axis Vec3
---@param maxAngle number
function sage.ik.SetAim(entity, goal, axis, maxAngle) end
---@param entity Entity
---@param goal number
---@param normal Vec3
function sage.ik.SetAlign(entity, goal, normal) end
---@param entity Entity
---@param on boolean
function sage.ik.SetEnabled(entity, on) end
---@param entity Entity
---@param goal number
---@param on boolean
---@param plantHeight number
---@param releaseTime number
function sage.ik.SetFootLock(entity, goal, on, plantHeight, releaseTime) end
---@param entity Entity
---@param goal number
---@param pole Vec3
function sage.ik.SetPole(entity, goal, pole) end
---@param entity Entity
---@param goal number
---@param target Vec3
function sage.ik.SetTarget(entity, goal, target) end
---@param entity Entity
---@param goal number
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
---@return number
function sage.input.Bind(action, keys) end
---@param action string
---@param positive string
---@param negative string
---@return number
function sage.input.BindAxis(action, positive, negative) end
---@param context string
---@param action string
---@param keys any
---@return number
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
---@param priority number
function sage.input.CreateContext(name, priority) end
---@param source string
---@return any
function sage.input.FindConflict(source) end
---@param index number
---@return boolean
function sage.input.GamepadConnected(index) end
---@param index number
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
function sage.light.SetSkyPreset(name) end
---@param t table
function sage.light.SetSun(t) end

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
function sage.math.Clamp(x, lo, hi) end
---@param a Vec3
---@param b Vec3
function sage.math.Cross(a, b) end
---@param rad number
function sage.math.Degrees(rad) end
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
---@param deg number
function sage.math.Radians(deg) end
---@return Vec2
function sage.math.RandomInsideUnitCircle() end
---@return Vec3
function sage.math.RandomInsideUnitSphere() end
---@return Vec3
function sage.math.RandomOnUnitSphere() end
---@param lo number
---@param hi number
function sage.math.RandomRange(lo, hi) end
---@param a Vec3
---@param b Vec3
---@param t number
---@return Vec3
function sage.math.Slerp(a, b, t) end
---@param edge0 number
---@param edge1 number
---@param x number
function sage.math.SmoothStep(edge0, edge1, x) end

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
---@param args any
---@return any
function sage.msg.Call(target, name, args) end
---@param target any
---@param name string
---@param data any
function sage.msg.Send(target, name, data) end

---@class sage.physics
sage.physics = {}

---Короткое имя того же модуля: `physics.X()` — это ТОТ ЖЕ объект,
---что `sage.physics`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.physics
physics = nil

---@param entity Entity
---@param radius number
---@param height number
function sage.physics.AddCharacter(entity, radius, height) end
---@param entity Entity
---@param impulse Vec3
function sage.physics.AddImpulse(entity, impulse) end
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
---@param velocity Vec3
---@param dt number
function sage.physics.MoveCharacter(entity, velocity, dt) end
---@param zone Entity
---@return table
function sage.physics.ObjectsInTrigger(zone) end
---@param center Vec3
---@param radius number
---@param mask number
---@return table
function sage.physics.OverlapSphere(center, radius, mask) end
---@param origin Vec3
---@param dir Vec3
---@param maxDistance number
---@param mask number
---@return any
function sage.physics.Raycast(origin, dir, maxDistance, mask) end
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
---@param layer number
function sage.physics.SetLayer(entity, layer) end
---@param entity Entity
---@param on boolean
function sage.physics.SetSensor(entity, on) end
---@param entity Entity
---@param mask number
function sage.physics.SetTriggerMask(entity, mask) end
---@param entity Entity
---@param v Vec3
function sage.physics.SetVelocity(entity, v) end
---@param entity Entity
---@return table
function sage.physics.TriggersOf(entity) end

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
---@param part number
---@param path string
function sage.render.SetSubmeshMaterial(entity, part, path) end
---@param entity Entity
---@return number
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
function sage.save.Delete(slot) end
function sage.save.Directory() end
---@param slot string
function sage.save.Exists(slot) end
---@param slot string
function sage.save.HasBackup(slot) end
---@param slot string
---@return any
function sage.save.Info(slot) end
---@param slot string
---@return any
function sage.save.Read(slot) end
---@param slot string
function sage.save.RestoreBackup(slot) end
---@return table
function sage.save.Slots() end
---@param slot string
---@return number
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
---@param id number
---@return Entity
function sage.scene.FindById(id) end
---@param name string
function sage.scene.Load(name) end
---@param id number
---@param path string
---@return boolean
function sage.scene.SavePrefab(id, path) end
---@param name string
---@return Entity
function sage.scene.Spawn(name) end
---@param path string
---@param at Vec3
---@return number
function sage.scene.SpawnPrefab(path, at) end
---@param pos Vec3
---@param scale number
---@return number
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

---@class sage.time
sage.time = {}

---Короткое имя того же модуля: `time.X()` — это ТОТ ЖЕ объект,
---что `sage.time`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.time
time = nil

---@param id number
function sage.time.Cancel(id) end
---@param intervalSeconds number
---@param fn function
---@return number
function sage.time.Repeat(intervalSeconds, fn) end
function sage.time.Scale() end
---@param seconds number
---@param fn function
---@return number
function sage.time.Schedule(seconds, fn) end
---@param scale number
function sage.time.SetScale(scale) end
---@param fn function
function sage.time.StartCoroutine(fn) end

---@class sage.tween
sage.tween = {}

---Короткое имя того же модуля: `tween.X()` — это ТОТ ЖЕ объект,
---что `sage.tween`, а не копия. Движок ставит его, только если имя
---свободно: игра, объявившая своё, остаётся при своём.
---@type sage.tween
tween = nil

function sage.tween.Active() end
---@param id any
function sage.tween.Cancel(id) end
function sage.tween.CancelAll() end
---@param entity Entity
---@param to Vec3
---@param dur number
---@param ease any
---@return any
function sage.tween.Color(entity, to, dur, ease) end
---@param entity Entity
---@param to Vec3
---@param dur number
---@param ease any
---@return any
function sage.tween.Move(entity, to, dur, ease) end
---@param entity Entity
---@param toEulerDeg Vec3
---@param dur number
---@param ease any
---@return any
function sage.tween.Rotate(entity, toEulerDeg, dur, ease) end
---@param entity Entity
---@param to Vec3
---@param dur number
---@param ease any
---@return any
function sage.tween.Scale(entity, to, dur, ease) end
---@param entity Entity
---@param to number
---@param dur number
---@param ease any
---@return any
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
---@return Vec2
function sage.ui.Cursor() end
---@param x number
---@param y number
---@param screenW number
---@param screenH number
---@return Entity
function sage.ui.ElementAt(x, y, screenW, screenH) end
---@param entity Entity
---@return number
function sage.ui.GetValue(entity) end
---@param name string
function sage.ui.HasIcon(name) end
---@return string
function sage.ui.HoveredAction() end
---@return table
function sage.ui.IconNames() end
---@param path string
function sage.ui.ImageSize(path) end
---@param entity Entity
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
---@param sharp boolean
function sage.ui.SetImage(entity, path, sharp) end
---@param entity Entity
---@param opts table
function sage.ui.SetLayout(entity, opts) end
---@param entity Entity
---@param l number
---@param t number
---@param r number
---@param b number
---@param scale number
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
---@param value number
function sage.ui.SetValue(entity, value) end
---@param name string
---@return number
function sage.ui.SpawnDemo(name) end
---@return number
function sage.ui.SpawnShowcase() end

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
---@type fun(...): any
ActiveTweens = sage.tween.Active
---@type fun(...): any
AddAnimatedModel = sage.anim.Add
---@type fun(...): any
AddBillboard = sage.fx.AddBillboard
---@type fun(...): any
AddCharacterController = sage.physics.AddCharacter
---@type fun(...): any
AddIKGoal = sage.ik.AddGoal
---@type fun(...): any
AddImpulse = sage.physics.AddImpulse
---@type fun(...): any
AddRenderTexture = sage.rt.Attach
---@type fun(...): any
AnimationCount = sage.anim.Count
---@type fun(...): any
AnimationDuration = sage.anim.Duration
---@type fun(...): any
AnimationFade = sage.anim.Fade
---@type fun(...): any
AnimationIndex = sage.anim.Index
---@type fun(...): any
AnimationNames = sage.anim.Names
---@type fun(...): any
AnimationTime = sage.anim.Time
---@type fun(...): any
ApplySkyPreset = sage.light.SetSkyPreset
---@type fun(...): any
BindAction = sage.input.Bind
---@type fun(...): any
BindActionIn = sage.input.BindIn
---@type fun(...): any
BindAxis = sage.input.BindAxis
---@type fun(...): any
BindVector = sage.input.BindVector
---@type fun(...): any
BoneAxis = sage.anim.BoneAxis
---@type fun(...): any
BonePosition = sage.anim.BonePosition
---@type fun(...): any
BorrowAnimations = sage.anim.Borrow
---@type fun(...): any
Broadcast = sage.msg.Broadcast
---@type fun(...): any
CallScript = sage.msg.Call
---@type fun(...): any
CancelTimer = sage.time.Cancel
---@type fun(...): any
CharacterFits = sage.physics.CharacterFits
---@type fun(...): any
CharacterState = sage.physics.CharacterState
---@type fun(...): any
Clamp = sage.math.Clamp
---@type fun(...): any
ClearJointPoses = sage.anim.ClearJointPoses
---@type fun(...): any
ClearMaterialShader = sage.render.ClearMaterialShader
---@type fun(...): any
ClearShaderParams = sage.render.ClearShaderParams
---@type fun(...): any
ClearUIImage = sage.ui.ClearImage
---@type fun(...): any
ClearUILayout = sage.ui.ClearLayout
---@type fun(...): any
Collisions = sage.physics.Collisions
---@type fun(...): any
CreateInputContext = sage.input.CreateContext
---@type fun(...): any
CreateParticleStream = sage.fx.CreateStream
---@type fun(...): any
Cross = sage.math.Cross
---@type fun(...): any
DebugViews = sage.render.DebugViews
---@type fun(...): any
Degrees = sage.math.Degrees
---@type fun(...): any
DeleteSave = sage.save.Delete
---@type fun(...): any
DestroyObject = sage.scene.Destroy
---@type fun(...): any
DisablePlanarReflection = sage.reflect.DisablePlanar
---@type fun(...): any
EmitEvent = sage.events.Emit
---@type fun(...): any
EmitFromObject = sage.fx.EmitFrom
---@type fun(...): any
EmitParticles = sage.fx.Emit
---@type fun(...): any
EventCount = sage.events.Count
---@type fun(...): any
FindObject = sage.scene.Find
---@type fun(...): any
FindObjectById = sage.scene.FindById
---@type fun(...): any
GenerateTexture = sage.texture.Generate
---@type fun(...): any
GetAxis = sage.input.Axis
---@type fun(...): any
GetCamera = sage.camera.Get
---@type fun(...): any
GetDebugView = sage.render.GetDebugView
---@type fun(...): any
GetLighting = sage.light.Get
---@type fun(...): any
GetMaterial = sage.render.GetMaterial
---@type fun(...): any
GetMaterialParam = sage.render.GetMaterialParam
---@type fun(...): any
GetMouseDelta = sage.input.MouseDelta
---@type fun(...): any
GetMousePosition = sage.input.MousePosition
---@type fun(...): any
GetScrollDelta = sage.input.ScrollDelta
---@type fun(...): any
GetUIElementAt = sage.ui.ElementAt
---@type fun(...): any
GetUIValue = sage.ui.GetValue
---@type fun(...): any
GetVector = sage.input.Vector
---@type fun(...): any
GetVelocity = sage.physics.GetVelocity
---@type fun(...): any
GroundNormal = sage.physics.GroundNormal
---@type fun(...): any
HasAction = sage.input.Has
---@type fun(...): any
HasIcon = sage.ui.HasIcon
---@type fun(...): any
HasSave = sage.save.Exists
---@type fun(...): any
HasSaveBackup = sage.save.HasBackup
---@type fun(...): any
IKLocked = sage.ik.Locked
---@type fun(...): any
IconNames = sage.ui.IconNames
---@type fun(...): any
ImageSize = sage.ui.ImageSize
---@type fun(...): any
IntersectRayPlane = sage.math.IntersectRayPlane
---@type fun(...): any
IntersectRaySphere = sage.math.IntersectRaySphere
---@type fun(...): any
IsActionDown = sage.input.IsDown
---@type fun(...): any
IsGrounded = sage.physics.IsGrounded
---@type fun(...): any
IsInTrigger = sage.physics.IsInTrigger
---@type fun(...): any
IsInputContextEnabled = sage.input.IsContextEnabled
---@type fun(...): any
IsMouseCaptured = sage.input.IsMouseCaptured
---@type fun(...): any
IsPlaying = sage.audio.IsPlaying
---@type fun(...): any
JointIndex = sage.anim.JointIndex
---@type fun(...): any
JointNames = sage.anim.JointNames
---@type fun(...): any
LaunchArg = sage.app.Arg
---@type fun(...): any
LaunchFlag = sage.app.Flag
---@type fun(...): any
LensFlareEnabled = sage.lensflare.Enabled
---@type fun(...): any
LoadGame = sage.save.Read
---@type fun(...): any
LoadInputMapping = sage.input.LoadMapping
---@type fun(...): any
LoadParticleEffect = sage.fx.LoadEffect
---@type fun(...): any
LoadScene = sage.scene.Load
---@type fun(...): any
LoadUISlice = sage.ui.LoadSlice
---@type fun(...): any
MaterialOf = sage.render.MaterialOf
---@type fun(...): any
MoveCharacter = sage.physics.MoveCharacter
---@type fun(...): any
NewMaterial = sage.render.NewMaterial
---@type fun(...): any
ObjectsInTrigger = sage.physics.ObjectsInTrigger
---@type fun(...): any
OffEvent = sage.events.Off
---@type fun(...): any
OnEvent = sage.events.On
---@type fun(...): any
OnceEvent = sage.events.Once
---@type fun(...): any
OverlapSphere = sage.physics.OverlapSphere
---@type fun(...): any
Play = sage.audio.Play
---@type fun(...): any
PlayAnimation = sage.anim.Play
---@type fun(...): any
PlayAnimationNamed = sage.anim.PlayNamed
---@type fun(...): any
PlayMusic = sage.audio.PlayMusic
---@type fun(...): any
PlayParticles = sage.fx.Play
---@type fun(...): any
PlaySound = sage.audio.PlaySound
---@type fun(...): any
PlaySound3D = sage.audio.PlaySound3D
---@type fun(...): any
Radians = sage.math.Radians
---@type fun(...): any
RandomInsideUnitCircle = sage.math.RandomInsideUnitCircle
---@type fun(...): any
RandomInsideUnitSphere = sage.math.RandomInsideUnitSphere
---@type fun(...): any
RandomOnUnitSphere = sage.math.RandomOnUnitSphere
---@type fun(...): any
RandomRange = sage.math.RandomRange
---@type fun(...): any
Raycast = sage.physics.Raycast
---@type fun(...): any
RebindAction = sage.input.Rebind
---@type fun(...): any
RefreshRenderTexture = sage.rt.Refresh
---@type fun(...): any
ReloadMaterial = sage.render.ReloadMaterial
---@type fun(...): any
RemoveBillboard = sage.fx.RemoveBillboard
---@type fun(...): any
RemoveParticleStream = sage.fx.RemoveStream
---@type fun(...): any
RenderTextureReady = sage.rt.Ready
---@type fun(...): any
Repeat = sage.time.Repeat
---@type fun(...): any
ResolveMaterialTextures = sage.render.ResolveMaterialTextures
---@type fun(...): any
RestoreSaveBackup = sage.save.RestoreBackup
---@type fun(...): any
SaveDirectory = sage.save.Directory
---@type fun(...): any
SaveGame = sage.save.Write
---@type fun(...): any
SaveInfo = sage.save.Info
---@type fun(...): any
SaveInputMapping = sage.input.SaveMapping
---@type fun(...): any
SaveMaterial = sage.render.SaveMaterial
---@type fun(...): any
SavePrefab = sage.scene.SavePrefab
---@type fun(...): any
SaveSlots = sage.save.Slots
---@type fun(...): any
SaveVersion = sage.save.Version
---@type fun(...): any
Schedule = sage.time.Schedule
---@type fun(...): any
SeekAnimation = sage.anim.Seek
---@type fun(...): any
SendMessage = sage.msg.Send
---@type fun(...): any
SetActionTrigger = sage.input.SetTrigger
---@type fun(...): any
SetAnimationLoop = sage.anim.SetLoop
---@type fun(...): any
SetAnimationPlaying = sage.anim.SetPlaying
---@type fun(...): any
SetAnimationSpeed = sage.anim.SetSpeed
---@type fun(...): any
SetBillboardPosition = sage.fx.SetBillboardPosition
---@type fun(...): any
SetBillboardTint = sage.fx.SetBillboardTint
---@type fun(...): any
SetBillboardVisible = sage.fx.SetBillboardVisible
---@type fun(...): any
SetCharacterPosition = sage.physics.SetCharacterPosition
---@type fun(...): any
SetCharacterShape = sage.physics.SetCharacterShape
---@type fun(...): any
SetCharacterWorld = sage.physics.SetCharacterWorld
---@type fun(...): any
SetClip = sage.audio.SetClip
---@type fun(...): any
SetDebugView = sage.render.SetDebugView
---@type fun(...): any
SetGravity = sage.physics.SetGravity
---@type fun(...): any
SetIKAim = sage.ik.SetAim
---@type fun(...): any
SetIKAlign = sage.ik.SetAlign
---@type fun(...): any
SetIKEnabled = sage.ik.SetEnabled
---@type fun(...): any
SetIKFootLock = sage.ik.SetFootLock
---@type fun(...): any
SetIKPole = sage.ik.SetPole
---@type fun(...): any
SetIKTarget = sage.ik.SetTarget
---@type fun(...): any
SetIKWeight = sage.ik.SetWeight
---@type fun(...): any
SetInputContextEnabled = sage.input.SetContextEnabled
---@type fun(...): any
SetJointRotation = sage.anim.SetJointRotation
---@type fun(...): any
SetLensFlare = sage.lensflare.Set
---@type fun(...): any
SetMasterVolume = sage.audio.SetMasterVolume
---@type fun(...): any
SetMaterial = sage.render.SetMaterial
---@type fun(...): any
SetMaterialParam = sage.render.SetMaterialParam
---@type fun(...): any
SetMaterialShader = sage.render.SetMaterialShader
---@type fun(...): any
SetMeshCapsule = sage.render.SetMeshCapsule
---@type fun(...): any
SetMeshCone = sage.render.SetMeshCone
---@type fun(...): any
SetMeshCube = sage.render.SetMeshCube
---@type fun(...): any
SetMeshCylinder = sage.render.SetMeshCylinder
---@type fun(...): any
SetMeshModel = sage.render.SetMeshModel
---@type fun(...): any
SetMeshNone = sage.render.SetMeshNone
---@type fun(...): any
SetMeshPlane = sage.render.SetMeshPlane
---@type fun(...): any
SetMeshSphere = sage.render.SetMeshSphere
---@type fun(...): any
SetMouseCaptured = sage.input.SetMouseCaptured
---@type fun(...): any
SetParticleStreamActive = sage.fx.SetStreamActive
---@type fun(...): any
SetParticleStreamPosition = sage.fx.SetStreamPosition
---@type fun(...): any
SetPauseMenu = sage.game.SetPauseMenu
---@type fun(...): any
SetPhysicsLayer = sage.physics.SetLayer
---@type fun(...): any
SetPitch = sage.audio.SetPitch
---@type fun(...): any
SetPlanarReflection = sage.reflect.SetPlanar
---@type fun(...): any
SetPlanarReflectionScale = sage.reflect.SetPlanarScale
---@type fun(...): any
SetReflectionIntensity = sage.reflect.SetIntensity
---@type fun(...): any
SetReflectionsEnabled = sage.reflect.SetEnabled
---@type fun(...): any
SetRootMotion = sage.anim.SetRootMotion
---@type fun(...): any
SetSensor = sage.physics.SetSensor
---@type fun(...): any
SetShaderParam = sage.render.SetShaderParam
---@type fun(...): any
SetSubmeshMaterial = sage.render.SetSubmeshMaterial
---@type fun(...): any
SetSun = sage.light.SetSun
---@type fun(...): any
SetTriggerMask = sage.physics.SetTriggerMask
---@type fun(...): any
SetUICanvas = sage.ui.SetCanvas
---@type fun(...): any
SetUIImage = sage.ui.SetImage
---@type fun(...): any
SetUILayout = sage.ui.SetLayout
---@type fun(...): any
SetUIPreset = sage.ui.Preset
---@type fun(...): any
SetUISlice = sage.ui.SetSlice
---@type fun(...): any
SetUISliceDrawsCenter = sage.ui.SetSliceDrawsCenter
---@type fun(...): any
SetUISliceTiled = sage.ui.SetSliceTiled
---@type fun(...): any
SetUISprite = sage.ui.SetSprite
---@type fun(...): any
SetUIValue = sage.ui.SetValue
---@type fun(...): any
SetVelocity = sage.physics.SetVelocity
---@type fun(...): any
SetVolume = sage.audio.SetVolume
---@type fun(...): any
SetVolumetrics = sage.volumetric.Set
---@type fun(...): any
SetWaterReflection = sage.reflect.SetWater
---@type fun(...): any
Slerp = sage.math.Slerp
---@type fun(...): any
SmoothStep = sage.math.SmoothStep
---@type fun(...): any
SpawnObject = sage.scene.Spawn
---@type fun(...): any
SpawnPrefab = sage.scene.SpawnPrefab
---@type fun(...): any
SpawnRagdoll = sage.scene.SpawnRagdoll
---@type fun(...): any
SpawnUIDemo = sage.ui.SpawnDemo
---@type fun(...): any
SpawnUIShowcase = sage.ui.SpawnShowcase
---@type fun(...): any
StartCoroutine = sage.time.StartCoroutine
---@type fun(...): any
Stop = sage.audio.Stop
---@type fun(...): any
StopMusic = sage.audio.StopMusic
---@type fun(...): any
StopParticles = sage.fx.Stop
---@type fun(...): any
SubmeshCount = sage.render.SubmeshCount
---@type fun(...): any
TriggersOf = sage.physics.TriggersOf
---@type fun(...): any
TweenCancel = sage.tween.Cancel
---@type fun(...): any
TweenCancelAll = sage.tween.CancelAll
---@type fun(...): any
TweenColor = sage.tween.Color
---@type fun(...): any
TweenMove = sage.tween.Move
---@type fun(...): any
TweenRotate = sage.tween.Rotate
---@type fun(...): any
TweenScale = sage.tween.Scale
---@type fun(...): any
TweenUIValue = sage.tween.UIValue
---@type fun(...): any
UIClickedAction = sage.ui.ClickedAction
---@type fun(...): any
UICursor = sage.ui.Cursor
---@type fun(...): any
UIHoveredAction = sage.ui.HoveredAction
---@type fun(...): any
UIMouseDown = sage.ui.MouseDown
---@type fun(...): any
UIPressedAction = sage.ui.PressedAction
---@type fun(...): any
UIReleasedAction = sage.ui.ReleasedAction
---@type fun(...): any
UIScreenSize = sage.ui.ScreenSize
---@type fun(...): any
VolumetricsEnabled = sage.volumetric.Enabled
---@type fun(...): any
WasActionPressed = sage.input.WasPressed
---@type fun(...): any
WasActionReleased = sage.input.WasReleased
---@type fun(...): any
WasActionTriggered = sage.input.Triggered
---@type fun(...): any
log = sage.core.log
---@type fun(...): any
logError = sage.core.logError
---@type fun(...): any
logWarn = sage.core.logWarn

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

# SAGE Lua

SAGE Lua — Lua с типами, классами и аннотациями для скриптов игры. Файлы
`.slua` компилирует сам движок при загрузке скрипта: отдельного шага сборки
нет. Сохранили файл — Play и горячая перезагрузка подхватывают его так же, как
`.lua`.

Расширение — **`.slua`**, а не `.sage`: `.sage` уже занят файлами сцен, и
скрипт с тем же расширением слот и панель ассетов не отличили бы от сцены.

Обычный Lua внутри `.slua` работает как есть: слова SAGE (`class`, `enum`,
`event`, `const`, `import`…) — ключевые только там, где начинают объявление.
Поэтому `type(x)` или поле `self.event` не ломаются.

## Конвейер

```
.slua → лексер → парсер → проверка типов → генератор Lua → runtime SAGE
        (sage/lang/SageLexer, SageParser, SageCheck, SageCodegen, SagePrelude)
```

- Сгенерированный Lua стоит **на тех же строках**, что исходник. Ошибка во
  время игры («attempt to index nil») указывает на строку `.slua` без карты
  строк.
- Ошибка компиляции не запускает скрипт. Консоль получает файл, строку и
  текст в таком виде:

  ```
  Scripts/Player.slua:5:22
  Ошибка типа:
  Ожидалось значение number.
  Получено значение string.
  Поле: Player.speed
  Получено: "fast"
  ```
- Результат компиляции кэшируется по пути и тексту: сто врагов с одним
  скриптом проверяются один раз.
- Runtime (`SagePrelude.cpp`) ставится один раз на состояние Lua, в глобальную
  таблицу `__sage`. Таймеры, `await` и вспомогательные компоненты идут кадром
  движка (`Backend::Tick`). Stop/Reset очищает задачи, таймеры и подписки.

## Типы

```lua
local a = 1                -- обычный Lua: без проверки
local hp: number = 100     -- проверяется
const MAX: integer = 3     -- нельзя присвоить ещё раз
var name: string? = nil    -- может быть nil
local speed := 5           -- тип выводится (integer расширяется до number)
local ids: number[] = {}
local byName: map<string, Enemy> = {}
type Id = number | string  -- псевдоним, объединение
```

Типизация постепенная. Значение без типа — `any`. Экземпляр класса (и
значение enum) тип помнит даже без аннотации: `local p = Player()` и затем
`p:hit("x")` проверяются. Сужения по `if x ~= nil` нет: `T?` присваивается в
`T` без ошибки.

## Функции

```lua
function damage(base: number, mult: number = 1): number
    return base * mult
end
local double = (x: number) => x * 2
function sum(...values: number[]): number ... end
function minMax(a: number, b: number): (number, number) ... end
```

## Классы

```lua
class Enemy extends Component
    public health: number = 100
    private target: Entity? = nil
    protected static count: integer = 0

    function init(hp: number)     -- конструктор (для обычных классов)
        self.health = hp
    end

    override function onUpdate(dt: number) ... end
    final function kill() ... end
end
```

- `public`/`private`/`protected` проверяются при компиляции.
- `override` обязателен при переопределении метода родителя. Хуки
  `Component` (`onUpdate`…) переопределяются без него, но их подпись
  сверяется.
- `super.method(...)` вызывает метод родителя.
- `final class` не наследуется, `final function` не переопределяется.
- Есть интерфейсы (`interface`, `implements`), `enum`, `struct` и
  обобщения: `class Pool<T>`, `Pool<Enemy>()`.

## Аннотации

Аннотация относится к **следующему объявлению**. Аннотация перед чем-то
другим — синтаксическая ошибка.

| Аннотация | Где | Что делает |
|---|---|---|
| `@serializable` | класс | помечает класс сохраняемым (метаданные) |
| `@require(RigidBody)` | компонент | при старте добавляет недостающий компонент движка |
| `@server`, `@client` | класс, метод | сторона (метаданные) |
| `@field(editable, name, tooltip, range = {min, max, step}, replicated)` | поле | поле в инспекторе |
| `@method(deprecated = "...")` | метод | предупреждение при вызове |

Поля `@field(editable = true)` главного компонента файла видны в
инспекторе: с подписью, подсказкой и диапазоном. Значение из сцены сильнее
умолчания. Диапазон проверяется: `min > max` или умолчание вне диапазона —
ошибка. `replicated` пока только метаданные: сеть его не читает.

## Компоненты

```lua
class PlayerController extends Component
    function onAwake() end            -- один раз, перед onStart
    function onStart() end
    function onUpdate(dt: number) end
    function onFixedUpdate(dt: number) end
    function onCollisionEnter(other) end
    function onTriggerEnter(other) end
    function onDestroy() end
end
```

- Главный компонент файла — класс, чьё имя совпадает с именем файла. Если
  такого нет — последний объявленный компонент.
- `self.entity`, `self.transform` — объект и его трансформ.
- `entity:addComponent(Класс)`, `getComponent`, `hasComponent` и
  `removeComponent` принимают класс SAGE, компонент движка
  (`CharacterController`, `AudioSource`…) или имя строкой.
- **Ограничение.** Движок держит один скрипт на объект. Первый
  `addComponent` скриптового класса ставит настоящий `ScriptComponent` (его
  видно в инспекторе, он сохраняется со сценой). Следующие — вспомогательные:
  их ведёт runtime SAGE. Они получают `onUpdate`, `onLateUpdate`, хуки
  столкновений и `onDestroy`, но не сохраняются со сценой.
- `onAwake` вызывается при первом `Start` объекта, сразу перед `onStart`.
- При уничтожении объекта всё, чем владел экземпляр, снимается само:
  подписки на события, таймеры и задачи.

## События

```lua
event PlayerDied(score: number)

Events.on(PlayerDied, function(score: number) ... end)
Events.once(PlayerDied, ...)
Events.emit(PlayerDied, 10)          -- аргументы проверяются по объявлению
local h = Events.on(...); h.disconnect()
```

Отправка безопасна посреди кадра: список обработчиков снимается до вызовов.
Ошибка в одном обработчике не мешает остальным. События SAGE уходят и в шину
движка: их слышат обычные `.lua` и связи инспектора. Обратного пути нет:
событие, отправленное из обычного `.lua`, обработчик SAGE не услышит.

## Ввод

`Input.isDown/isPressed/isReleased(действие или Key.X)`,
`Input.getAxis("Horizontal")`, `Input.getAxis2D("Move")` и
`Input.bind("Jump", Key.Space)` (или `{negative = ..., positive = ...}`,
`{up, down, left, right}`). Без объявленного действия `Horizontal`,
`Vertical` и `Move` читают WASD и стрелки.

## Асинхронность

```lua
async function spawnWave()
    await Time.wait(1.5)
    await Time.nextFrame()
    await Time.waitUntil(function() return self.ready end)
    local tex = await Assets.load("Textures/Hero")
end
Timer.after(2, fn); local t = Timer.every(0.5, fn); t:cancel()
```

`await` вне `async`-функции — ошибка компиляции. `async`-функция возвращает
задачу: `task:isDone()`, `task:result()`, `task:cancel()`.

## Модули

```lua
import { Player, MAX_HP } from "Game/Player"
import { Input, Events } from "SAGE/Core"
export class Player ... end
export const MAX_HP: number = 100
```

Модуль ищется от папки файла вверх и от корня проекта (`Scripts/`,
`assets/scripts/`, `Modules/`…), как `.slua` или `.lua`. В собранной игре —
в пакете. Циклический импорт — ошибка со всей цепочкой файлов.

## Result, Log, математика

- `Result.ok(v)`/`Result.err(e)`, `r:isOk()`, `r:value()`, `r:unwrapOr(x)`.
- `Log.info/warn/error` пишут в консоль редактора.
- `Vector3.zero/one/up/forward` возвращают каждый раз новый вектор.
  `v:normalized()`, `v:lengthSquared()`, `Vector3.MoveTowards(...)`,
  `Mathf.clamp/lerp/moveTowards`.
- Сцена: `Scene.findEntity`, `Scene.instantiate`, `Scene.load`.
- Физика: `Physics.raycast`, `Physics.overlapSphere`.

## Пример

`editor/assets/scripts/examples/PlayerController.slua`. Новый скрипт:
ПКМ в панели ассетов → «Новый скрипт SAGE Lua (.slua)».

## Проверки

- `tests/test_sagelua.cpp` — компилятор и runtime на голом Lua.
- `tests/test_sagelua_engine.cpp` — `.slua` на объекте сцены через
  `ScriptingSystem`.
- Самопроверка редактора компилирует заготовку нового скрипта.

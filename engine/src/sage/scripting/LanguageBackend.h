#pragma once
#include <memory>
#include <string>
#include <vector>

#include "sage/scripting/ScriptTypes.h"

class Scene;
class GameObject;
class Camera;
class PhysicsScene;
class AudioEngine;
namespace sage::input { class InputSystem; }

// ---------------------------------------------------------------------------
// ЯЗЫК КАК СМЕННАЯ ДЕТАЛЬ.
//
// LanguageBackend — всё, что движок знает про конкретный язык скриптов:
// «умеешь такие файлы?», «собери экземпляр из этого текста», «позови у него
// Update», «положи в него вот эти публичные поля». Ни одного понятия Lua здесь
// нет — поэтому C#, JavaScript или Python добавляются НОВЫМ бэкендом, без
// единой правки в ECS, компонентах, инспекторе и формате сцены.
//
// ПОЧЕМУ ИМЕННО ТАКАЯ ГРАНИЦА. Всё, что общее для всех языков, — чтение файла,
// выбор бэкенда по расширению, порядок хуков в кадре, горячая перезагрузка,
// гашение потока одинаковых ошибок, хранение публичных полей в сцене — живёт
// ВЫШЕ (ScriptRuntime/ScriptingSystem) и написано один раз. Бэкенду остаётся
// то, что действительно зависит от языка: компиляция, вызов, преобразование
// значений. Иначе второй язык означал бы вторую копию всего перечисленного.
//
// ПОЧЕМУ ЗНАЧЕНИЯ — sage::vars::Value. Публичное поле скрипта видит инспектор,
// пишет сериализатор сцены и читает рантайм. Свой тип значения у каждого языка
// означал бы перевод «язык → движок → инспектор» в каждом бэкенде и свой набор
// ошибок в каждом.
// ---------------------------------------------------------------------------
namespace sage::scripting {

// Что движок отдаёт скриптам. Невладеющие указатели: их держит хозяин кадра
// (рантайм игры, Play-режим редактора), а не язык. Любой может быть nullptr —
// тогда соответствующий раздел API обязан сказать «не привязано» понятной
// ошибкой, а не упасть.
struct ScriptServices {
    Scene* ScenePtr = nullptr;
    sage::input::InputSystem* Input = nullptr;
    PhysicsScene* Physics = nullptr;
    AudioEngine* Audio = nullptr;
    Camera* MainCamera = nullptr;
    // Часы кадра: Time.deltaTime и прочее. Владеет ими ScriptingSystem —
    // один источник времени на все языки сразу.
    ScriptClock* Clock = nullptr;
    // Папки поиска модулей (require и его аналоги в других языках).
    std::vector<std::string> ModulePaths;
};

class LanguageBackend {
public:
    virtual ~LanguageBackend() = default;

    // Имя языка для сообщений: "Lua", "C#". Оно же — то, что видит человек в
    // консоли, поэтому пишется так, как язык называется, а не как файл.
    virtual const char* Name() const = 0;

    // Берётся ли этот файл. Расширение приходит В НИЖНЕМ РЕГИСТРЕ и с точкой.
    virtual bool Handles(const std::string& extension) const = 0;

    // Привязка служб движка. Зовётся до первого Create и при смене сцены.
    virtual void Bind(const ScriptServices& services) = 0;

    // --- Публичные поля ------------------------------------------------------
    //
    // РАЗБОР ТЕКСТА, А НЕ ВЫПОЛНЕНИЕ ФАЙЛА, и это требование, а не совет:
    // инспектор показывает поля, ПОКА ИГРА НЕ ЗАПУЩЕНА. Выполнить ради списка
    // переменных чужой файл нельзя — он на верхнем уровне может открыть сокет,
    // начать грузить уровень или уронить редактор ошибкой в коде, до которого
    // в игре дело дошло бы через час.
    virtual sage::vars::Table ParseFields(const std::string& source) const = 0;

    // --- Жизнь экземпляра ----------------------------------------------------

    // Компилирует исходник (с кэшем внутри бэкенда) и создаёт экземпляр,
    // привязанный к объекту. kInvalidInstance + заполненный err — ошибка
    // компиляции или выполнения тела файла; хозяин обязан ОСТАВИТЬ старый
    // экземпляр работать (см. ScriptRuntime::ReloadChanged).
    virtual InstanceId Create(const ScriptSource& source, GameObject owner, ScriptError& err) = 0;

    // Уничтожает экземпляр. Повторный вызов — не ошибка.
    virtual void Destroy(InstanceId id) = 0;

    // Объявлен ли у экземпляра этот хук. Движок обязан спрашивать: требовать
    // от каждого скрипта реализовать все десять методов — значит обязать
    // писать девять пустых.
    virtual bool Has(InstanceId id, Hook hook) const = 0;

    // Хук со временем кадра (Start/Update/FixedUpdate/LateUpdate/OnEnable/…).
    virtual bool Call(InstanceId id, Hook hook, float dt, ScriptError& err) = 0;

    // Хук с другим объектом (столкновения и зоны).
    virtual bool CallWith(InstanceId id, Hook hook, GameObject other, ScriptError& err) = 0;

    // Хук со строкой (события анимации).
    virtual bool CallNamed(InstanceId id, Hook hook, const std::string& name, ScriptError& err) = 0;

    // Произвольный метод экземпляра — то, чем скрипты разговаривают друг с
    // другом (`enemy:Call("TakeDamage", 20)`). Значения — общий словарь движка,
    // поэтому так сможет позвать и C++, и скрипт на другом языке.
    virtual bool Invoke(InstanceId id, const std::string& method,
                        const std::vector<sage::vars::Value>& args, ScriptError& err) = 0;

    // Кладёт значения публичных полей в экземпляр (до Start и после правки в
    // инспекторе). Поля вида «ссылка на объект/компонент» бэкенд обязан отдать
    // скрипту УЖЕ объектом, а не номером.
    virtual void ApplyFields(InstanceId id, const sage::vars::Table& fields) = 0;

    // Тик самого рантайма языка: таймеры, корутины, сборка мусора. Хуки
    // экземпляров зовёт хозяин, а не это.
    virtual void Tick(float dt) { (void)dt; }

    // Всё снести (смена сцены, Stop). После этого бэкенд обязан быть готов
    // принимать Create заново — без утечек и без памяти о прежних экземплярах.
    virtual void Reset() = 0;
};

} // namespace sage::scripting

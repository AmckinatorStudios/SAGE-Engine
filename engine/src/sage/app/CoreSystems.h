#pragma once
#include "sage/core/SystemScheduler.h"

// Эти классы живут в ГЛОБАЛЬНОМ пространстве имён, а не в sage — объявляем их
// здесь же, иначе объявление внутри namespace sage создало бы другие типы.
class ScriptEngine;
namespace sage::scripting { class ScriptingSystem; }
class PhysicsScene;
class ParticleSystem;
class AudioEngine;

// ---------------------------------------------------------------------------
// СТАНДАРТНЫЙ СОСТАВ КАДРА — слой app, а не core.
//
// Сам планировщик (core/SystemScheduler.h) не знает ни одной подсистемы: он
// гоняет функции по стадиям. Здесь он НАПОЛНЯЕТСЯ скриптами, физикой, звуком,
// частицами — а это уже сборка программы из подсистем, и место ей над ними.
// Пока это лежало в core, базовый слой движка зависел от скриптов, физики и
// звука, и собрать или проверить его отдельно было нельзя.
// ---------------------------------------------------------------------------
namespace sage {

// --- Стандартный набор систем движка ------------------------------------------

// Что подключать. Нулевой указатель — систему не регистрировать: у редактора в
// режиме правки нет ни скриптов, ни физики, и это нормальный случай, а не
// ошибка.
struct CoreSystems {
    // Прежний движок Lua: уровневые скрипты, таймеры, корутины, старый API.
    ScriptEngine* Scripts = nullptr;
    // Система скриптинга объектов (Start/Update/FixedUpdate/LateUpdate и
    // события столкновений). Язык ей неизвестен — см.
    // sage/scripting/ScriptingSystem.h.
    sage::scripting::ScriptingSystem* Scripting = nullptr;
    PhysicsScene* Physics = nullptr;
    ParticleSystem* Particles = nullptr;
    AudioEngine* Audio = nullptr;
    bool Animation = true;    // анимация не требует объекта — только сцену
    // Ход времени суток (DayNightCycle сцены). Только в ИГРЕ: в режиме правки
    // время стоит там, куда его поставили, иначе сцена менялась бы сама, пока
    // её настраивают.
    bool DayNight = false;
};

// Регистрирует стандартную последовательность движка. ЕДИНСТВЕННОЕ место, где
// записан порядок кадра: если он неверен, он неверен ровно здесь, а не в трёх
// файлах по-разному.
void RegisterCoreSystems(SystemScheduler& scheduler, const CoreSystems& systems);

} // namespace sage

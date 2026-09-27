#pragma once
#include <string>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <sol/sol.hpp>

#include "sage/anim/Tween.h"

// ---------------------------------------------------------------------------
// ТВИНЫ ГЛАЗАМИ LUA — общее для нынешнего API (Tween.to, lua/LuaApi_Tween.cpp)
// и прежнего (TweenMove, ScriptApi_Tween.cpp). Оба ведут в ОДИН проигрыватель
// сцены (Scene::Tweens); здесь — только перевод из Lua: кривая, значение,
// короткое имя свойства.
// ---------------------------------------------------------------------------
namespace sage::scripting::tween {

// Таблица Ease: Ease.Out, Ease.InOut, Ease.OutBack, Ease.Linear… и прежние
// имена (Ease.QuadOut, Ease.BackOut). Значения — строки ("quad-out"), их же
// понимает Tween.to напрямую.
sol::table MakeEaseTable(sol::state_view lua);

// Кривая из Lua: строка ("out", "back-out", Ease.OutBack), число прежнего
// перечисления (Ease.QuadOut старых игр) или таблица {x1, y1, x2, y2} — своя
// кривая. nil — fallback.
anim::Ease EaseFrom(const sol::object& o, const anim::Ease& fallback);

// Короткое имя свойства → ключ реестра по тому, что есть у сущности:
// "position" у элемента интерфейса — element.position, у объекта сцены —
// object.position; "color" — цвет заливки/картинки/текста или материала;
// "opacity", "scale", "size", "rotation", "text_size", "corner_radius",
// "intensity", "fov", "volume". Полный ключ ("fill.color") — как есть.
// Пусто — у сущности такого свойства нет.
std::string ResolveProperty(const entt::registry& reg, entt::entity e, const std::string& name);

// Значение из Lua (число, Vector2/3, Color) → vec4 под свойство: число на
// свойстве из трёх компонент — все три (`scale = 1.2`). false — не значение.
bool ValueFrom(const sol::object& o, int components, glm::vec4& out);

} // namespace sage::scripting::tween

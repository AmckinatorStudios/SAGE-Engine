#pragma once
#include "ScriptEngine.h"

// ---------------------------------------------------------------------------
// Внутренняя кухня привязок Lua, общая для файлов ScriptApi_*.cpp.
//
// Заголовок ВНУТРЕННИЙ: он лежит рядом с реализацией, а не в публичном API
// движка, и включать его игре незачем. Появился он ровно тогда, когда
// ScriptEngine.cpp разъехался по файлам: то, что раньше было безымянным
// пространством имён одного .cpp, теперь нужно двум, и дублировать шаблон в
// каждом значило бы завести две копии, которые однажды разойдутся.
// ---------------------------------------------------------------------------
namespace sage::scripting::detail {

// Навешивает на usertype GameObject единый набор аксессоров к компоненту C:
//   entity:HasX()    -> bool  (есть ли компонент)
//   entity:GetX()    -> C | nil (Unity-семантика: nil, если компонента нет)
//   entity:AddX()    -> C     (создаёт при отсутствии и отдаёт ссылкой для правки)
//   entity:RemoveX()          (снимает компонент)
// Так один шаблон закрывает Light/Camera/RigidBody/Collider/ParticleEmitter/…
// одинаковым, предсказуемым API — скрипт правит ЛЮБОЙ компонент ЛЮБОЙ сущности
// (в т.ч. чужой), то есть общается со всеми системами движка через ECS.
template <typename C>
void BindComponentAccessors(sol::usertype<GameObject>& t, const char* has,
                            const char* get, const char* add, const char* remove) {
    t[has] = [](GameObject& o) {
        return o.Valid() && o.Registry()->all_of<C>(o.Entity());
    };
    t[get] = [](GameObject& o) -> C* {
        return o.Valid() ? o.Registry()->try_get<C>(o.Entity()) : nullptr;
    };
    t[add] = [](GameObject& o) -> C& {
        if (!o.Valid()) throw std::runtime_error("Добавление компонента: невалидная сущность");
        return o.Registry()->get_or_emplace<C>(o.Entity());
    };
    t[remove] = [](GameObject& o) {
        if (o.Valid()) o.Registry()->remove<C>(o.Entity());
    };
}

} // namespace sage::scripting::detail

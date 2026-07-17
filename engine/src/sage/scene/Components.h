#pragma once
#include <string>
#include <memory>
#include <glm/glm.hpp>
#include "sage/scene/Transform.h"
#include "sage/render/Mesh.h"
#include "sage/render/Material.h"

// ---------------------------------------------------------------------------
// Компоненты ECS — простые data-структуры, навешиваемые на сущности (entity)
// в sage::Scene (поверх entt). Системы (см. sage/ecs/*) итерируют сущности по
// набору компонентов. Движок не зашивает «толстый» объект с фиксированными
// полями — состав сущности собирается из компонентов, что и делает архитектуру
// расширяемой: своя игра добавляет свои компоненты, не трогая ядро.
// ---------------------------------------------------------------------------

// Человекочитаемое имя сущности (для поиска, иерархии, инспектора редактора).
struct NameComponent {
    std::string Name;
};

// Стабильный целочисленный id сущности в пределах сцены. entt::entity несёт в
// себе биты версии и не годится как «простой id» для Lua/сериализации, поэтому
// движок раздаёт свои последовательные id (1,2,3...) и держит карту id->entity
// в Scene. Так сохраняется прежний контракт GameObject.Id / DestroyObject(id).
struct IdComponent {
    int Id = 0;
};

// Описание того, ИЗ ЧЕГО сделан меш — то, что реально сохраняется в файл сцены.
// Сам GPU-меш (Mesh) не сериализуется, он пересоздаётся при загрузке через
// ResourceManager на основе этого описания.
struct MeshRef {
    enum class Type { None, Cube, Model };
    Type type = Type::None;
    std::string path; // используется только при Type::Model

    bool operator==(const MeshRef& other) const {
        return type == other.type && path == other.path;
    }
};

// Визуальное представление сущности: чем она нарисована (Ref + runtime-меш на
// GPU) и каким цветом тонируется. Сущность без MeshPtr не рисуется (см.
// RenderSystem) — так же, как раньше пропускался GameObject без MeshComponent.
struct MeshRendererComponent {
    MeshRef Ref;
    glm::vec3 Color{1.0f, 1.0f, 1.0f};
    // Runtime-указатель на GPU-меш. Не сериализуется — заполняется
    // ResourceManager'ом на основе Ref при загрузке сцены или назначении меша.
    std::shared_ptr<Mesh> MeshPtr;

    // Материал (.sagemat) — переиспользуемое описание внешнего вида, общее
    // для всех сущностей с этим путём. Path сериализуется; Ptr — runtime,
    // восстанавливается ResourceManager::GetMaterial при загрузке сцены.
    // Назначенный материал ЗАМЕНЯЕТ Color (см. EffectiveColor ниже).
    std::string MaterialPath;
    std::shared_ptr<Material> MaterialPtr;
};

// Итоговый базовый цвет сущности для рендера: albedo материала, если материал
// назначен, иначе — прямой Color компонента. Единая точка выбора для
// редактора и игр (см. использование в EditorLayer/SandboxLayer/TestGame).
inline glm::vec3 EffectiveColor(const MeshRendererComponent& mr) {
    return mr.MaterialPtr ? mr.MaterialPtr->Albedo : mr.Color;
}

// Поведение сущности на Lua: путь к .lua файлу со стандартными хуками
// OnStart(entity)/OnUpdate(entity, dt) (см. sage/scripting/ScriptEngine.h).
// Сам компонент — только ДАННЫЕ (путь, сериализуется вместе со сценой);
// привязку к ScriptEngine выполняет рантайм: игра — при старте уровня,
// редактор — при входе в Play-режим.
struct ScriptComponent {
    std::string Path;
};

// Игровая камера сцены. Позицию и ориентацию задаёт Transform сущности
// (Scale не влияет). Панель Game редактора — и игры, которым это удобно —
// рендерят изображение от ПЕРВОЙ сущности с Primary == true; так камера
// становится частью сцены (сериализуется), а не хардкодом кода игры.
struct CameraComponent {
    float Fov = 60.0f;      // вертикальный угол обзора, градусы
    float NearClip = 0.1f;
    float FarClip = 200.0f;
    bool Primary = true;    // первая Primary-камера сцены — «главная»
};

// Точечный свет на сущности: позиция берётся из Transform (двигается гизмо,
// анимируется скриптами — как любой объект), затухание — из Range (см.
// PointLight в Light.h). Направленное «солнце» и фоновая засветка остаются
// НАСТРОЙКАМИ СЦЕНЫ (Scene::Lighting — их логично править как окружение, а не
// таскать по миру). Рендер собирает итоговое освещение кадра через
// sage::ecs::CollectLighting: окружение сцены + света-сущности.
struct LightComponent {
    glm::vec3 Color{1.0f, 0.9f, 0.7f};
    float Intensity = 1.5f;
    float Range = 12.0f; // дистанция затухания, единицы мира
};

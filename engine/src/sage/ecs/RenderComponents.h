#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include "sage/render/Mesh.h"
#include "sage/render/Material.h"

// ---------------------------------------------------------------------------
// Компоненты живут рядом со своей системой, а не в общем файле.
//
// Раньше всё это лежало в scene/Components.h — 583 строки, где соседствовали
// интерфейс, обратная кинематика, зонды отражений, физика, частицы и анимация.
// Такой файл растёт при КАЖДОЙ фиче, и это не эстетика: правка одного поля
// перекомпилирует всё, что вообще знает про ECS, а найти нужный компонент
// глазами можно только поиском. Здесь — только то, что читает и пишет одна
// система.
//
// scene/Components.h остался ЗОНТИКОМ: он включает эти файлы, и старый
// `#include "sage/scene/Components.h"` продолжает работать. Ломать три десятка
// включений ради перестановки — цена без выгоды.
//
// Система: sage/ecs/RenderSystem.h и sage/ecs/RenderBatch.h.
// ---------------------------------------------------------------------------

// Описание того, ИЗ ЧЕГО сделан меш — то, что реально сохраняется в файл сцены.
// Сам GPU-меш (Mesh) не сериализуется, он пересоздаётся при загрузке через
// ResourceManager на основе этого описания.
struct MeshRef {
    // Примитивы генерируются процедурно (Mesh::Create*), Model грузится из файла
    // по path. None — сущность без меша (камера/свет/пустышка), не рисуется.
    enum class Type { None, Cube, Sphere, Plane, Cylinder, Cone, Model };
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
    // Непрозрачность: 1 — обычный объект, меньше — полупрозрачный. Отдельным
    // полем, а не четвёртым каналом Color, намеренно: цвет объекта и его
    // прозрачность — разные вещи, их правят в разных местах и по разным
    // причинам, и старые сцены с vec3-цветом продолжают читаться как были.
    float Opacity = 1.0f;

    // Собственное свечение ОБЪЕКТА, отдельно от материала.
    //
    // Материал — общий ресурс: сделать один фонарь ярче другого через материал
    // значит завести материал на каждый фонарь. А светящиеся объекты в игре
    // именно такие: сотня одинаковых ламп, у каждой своя яркость, и меняется она
    // в рантайме (лампа гаснет, угли остывают). Поэтому свечение живёт и здесь.
    //
    // Складывается со свечением материала, а не заменяет его: материал задаёт
    // «каким светится этот предмет вообще», объект — «насколько именно этот».
    glm::vec3 Emissive{0.0f, 0.0f, 0.0f};
    float EmissiveStrength = 1.0f;
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

// Итоговая непрозрачность: как и с цветом, назначенный материал главнее — но
// множится на собственную прозрачность сущности, чтобы «затухание» одного
// объекта не требовало отдельной копии материала.
// Итоговое свечение: своё у объекта плюс материала. Единая точка, чтобы рендер,
// редактор и скрипты считали его одинаково.
inline glm::vec3 EffectiveEmissive(const MeshRendererComponent& mr) {
    glm::vec3 e = mr.Emissive * mr.EmissiveStrength;
    if (mr.MaterialPtr) e += mr.MaterialPtr->Emissive * mr.MaterialPtr->EmissiveStrength;
    return e;
}

inline float EffectiveOpacity(const MeshRendererComponent& mr) {
    return mr.MaterialPtr ? mr.MaterialPtr->Opacity * mr.Opacity : mr.Opacity;
}

// Порог, ниже которого объект считается полупрозрачным и уходит в отдельный
// проход. Не ноль: значение вроде 0.999 от анимации не должно каждый кадр
// перекидывать объект между проходами.
inline bool IsTransparent(const MeshRendererComponent& mr) {
    return EffectiveOpacity(mr) < 0.996f;
}

// Переопределение юниформ шейдера ДЛЯ ОДНОЙ СУЩНОСТИ. Материал общий: правка
// его Params меняет вид всех объектов с этим материалом сразу — а игре сплошь и
// рядом нужно обратное («мигает именно ЭТА кнопка», «эта доска подсвечена»).
// Значения отсюда накладываются поверх материаловых при отрисовке.
struct ShaderParamsComponent {
    std::unordered_map<std::string, ShaderParam> Params;
};

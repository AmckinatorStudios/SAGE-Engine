// ---------------------------------------------------------------------------
// EditorLayer — правка сцены: отмена, выделение, объекты.
//
// Отмена и повтор, выделение, создание и удаление объектов, префабы,
// выравнивание и подгонка камеры. Связывает их одно: каждое такое действие
// МЕНЯЕТ ДОКУМЕНТ и потому обязано попасть в историю отмены. Держать их рядом
// — единственный способ не забыть про снимок в новом действии.
//
// Часть класса EditorLayer: объявления методов остались в EditorLayer.h, здесь
// только тела. Разбит он ровно потому, что дорос до двух с половиной тысяч
// строк, в которых рядом лежали сборка игры, отмена правки и раскладка окон —
// три области, у которых нет ничего общего, кроме имени класса.
// ---------------------------------------------------------------------------
#include "EditorLayer.h"

#include <fstream>

#include "sage/vars/ScriptVars.h"
#include "sage/vars/VarsComponent.h"
#include "sage/assets/Pack.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <fstream>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API (создание раскладки по умолчанию)
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"

#include "EditorTheme.h"
#include "EditorIcons.h"
#include "ModelMaterialImport.h"
#include "sage/render/DebugView.h"
#include "sage/core/Application.h"
#include "sage/core/Paths.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Systems.h"
#include "sage/core/Version.h"
#include "sage/core/CrashHandler.h"
#include "sage/render/MeshRaycast.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/render/LightingUpload.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/render/ParticlePresets.h"
#include "sage/gi/GI.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/scene/CopyName.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/SceneSerializer.h"
#include "Localization.h"

namespace fs = std::filesystem;

namespace {

// Пересечение луча с произвольным AABB [bmin, bmax] в локальном пространстве
// объекта (slab-тест). Возвращает t входа (>=0) или отрицательное при промахе.
float RayBox(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& bmin, const glm::vec3& bmax) {
    glm::vec3 inv = 1.0f / rd; // IEEE inf при нулевой компоненте — slab-тест это переживает
    glm::vec3 t0 = (bmin - ro) * inv;
    glm::vec3 t1 = (bmax - ro) * inv;
    glm::vec3 tmin = glm::min(t0, t1), tmax = glm::max(t0, t1);
    float tNear = std::max({tmin.x, tmin.y, tmin.z});
    float tFar  = std::min({tmax.x, tmax.y, tmax.z});
    if (tNear > tFar || tFar < 0.0f) return -1.0f;
    return tNear >= 0.0f ? tNear : tFar;
}

// Луч vs единичный куб [-0.5,0.5]^3 — маркеры невидимых сущностей (камера/свет).
float RayUnitCube(const glm::vec3& ro, const glm::vec3& rd) {
    return RayBox(ro, rd, glm::vec3(-0.5f), glm::vec3(0.5f));
}

constexpr float kStatusBarHeight = 26.0f;

} // namespace


// Публичные переменные объекта = ЕГО значения + ОБЪЯВЛЕНИЕ его скрипта.
//
// Читается перед показом секции, каждый кадр показа. Дорого ли это: файл
// скрипта — это килобайты, разбор — один проход по строке, и делается он
// только пока секция открыта у ВЫБРАННОГО объекта. Кэш здесь был бы вреднее:
// .lua правят снаружи редактора (в чужом редакторе, git-ом), и переменная,
// добавленная минуту назад, обязана появиться сама — а не после
// переназначения файла, о котором никто не догадается.
void EditorLayer::MergeScriptVars(GameObject object) {
    if (!object.Valid()) return;
    entt::registry& reg = m_scene->Registry();
    const ScriptComponent* sc = reg.try_get<ScriptComponent>(object.Entity());
    if (!sc || sc->Path.empty()) return;

    // Путь в сцене относителен корню проекта — тем же способом, каким его
    // разрешают меш, материал и текстура.
    std::filesystem::path full = sc->Path;
    if (full.is_relative() && m_project.Loaded()) full = m_project.Dir() / full;
    std::error_code ec;
    if (!std::filesystem::exists(full, ec)) return;

    std::ifstream in(full, std::ios::binary);
    if (!in) return;
    const std::string source((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    const sage::vars::Table declaration = sage::vars::ParseDeclaration(source);
    if (declaration.Empty()) return;

    reg.get_or_emplace<VarsComponent>(object.Entity()).Values.MergeDeclaration(declaration);
}

// Стопки снимков и их правила живут в EditorHistory — здесь остаётся только
// то, чего история про редактор знать не должна: что Play-режим её отключает
// и что после удавшейся записи сцена считается изменённой (звёздочка в
// заголовке окна).
void EditorLayer::PushUndoSnapshot() {
    m_history.SetEnabled(!InPlayMode()); // правки в Play эфемерны — Stop их откатит
    if (m_history.Push()) MarkSceneDirty();
}

void EditorLayer::CommitPendingSnapshot() {
    m_history.SetEnabled(!InPlayMode());
    if (m_history.CommitPending()) MarkSceneDirty();
}

// Одна запись undo на всё перетаскивание DragFloat/набор текста: состояние
// «до» запоминается на активации виджета, в стек уходит на завершении правки.
void EditorLayer::TrackLastImGuiItem() {
    if (InPlayMode()) return;
    if (ImGui::IsItemActivated()) m_history.CapturePending();
    if (ImGui::IsItemDeactivatedAfterEdit()) CommitPendingSnapshot();
}

void EditorLayer::MarkSceneDirty() {
    m_sceneDirty = true;
    UpdateWindowTitle();
}

void EditorLayer::Undo() {
    m_history.SetEnabled(!InPlayMode());
    if (m_history.Undo()) MarkSceneDirty();
}

void EditorLayer::Redo() {
    m_history.SetEnabled(!InPlayMode());
    if (m_history.Redo()) MarkSceneDirty();
}

// ============================================================================
//  Сущности
// ============================================================================


// Готовый элемент интерфейса по имени пресета. Значения подобраны так, чтобы
// созданный элемент был СРАЗУ ВИДЕН и сразу делал то, что обещает названием:
// кнопка ловит мышь, полоса заполнена наполовину, поле ввода имеет подсказку.
// Ноль в размере или прозрачный цвет по умолчанию означали бы, что человек
// создал элемент и не увидел ничего.
GameObject EditorLayer::CreateUIEntity(const std::string& preset) {
    // ПУСТОЙ объект: элемент интерфейса рисует система UI, а не меш. С
    // CreateObject у каждой кнопки и надписи висел компонент «Меш» с моделью,
    // цветом и тенями — настройками, которых у прямоугольника на экране нет.
    GameObject obj = m_scene->CreateEmptyObject(preset);
    entt::registry& reg = m_scene->Registry();

    // Новый элемент становится ДОЧЕРНИМ к выделенному элементу интерфейса.
    //
    // Интерфейс собирается из вложенных прямоугольников: панель, а в ней
    // надпись, кнопка и полоса. Раньше каждый созданный элемент вставал в
    // корень, то есть отсчитывался от края ЭКРАНА, и собрать панель означало
    // создать элементы, а потом перетащить каждый в иерархии на панель, помня,
    // что до этого они лежали не там. Самый частый шаг верстки требовал
    // отдельного ручного действия — и именно это ощущается как «неудобно
    // прикреплять».
    if (m_selection.Primary() >= 0) {
        GameObject sel = m_scene->Get(m_selection.Primary());
        if (sel.Valid() && sage::ui::IsElement(reg, sel.Entity())) {
            m_scene->SetParent(obj.Entity(), sel.Entity());
        }
    }

    // Что именно значит «кнопка» или «полоса», знает ДВИЖОК (sage/ui/UIPresets.h).
    // Раньше это знание жило только здесь, и получить кнопку можно было лишь
    // мышью в редакторе: скрипт, собирающий интерфейс на лету, повторял те же
    // семь присваиваний у себя.
    // Через СЦЕНУ: у заготовок есть дети (надпись на кнопке — отдельный
    // объект), а создать объект и назначить родителя умеет только сцена.
    if (!sage::ui::ApplyPreset(*m_scene, obj.Entity(), preset)) {
        // Имени заготовки нет. Раньше это РОНЯЛО РЕДАКТОР: заготовка не
        // применялась, компонента Transform у объекта не появлялось, а строчкой
        // ниже его брали через get<> — обращение к несуществующему компоненту,
        // то есть падение без единого сообщения. Достаточно было опечатки в
        // имени — в плагине, в скрипте или в самом редакторе.
        //
        // Пустой объект в сцене после отказа тоже не нужен: он ничего не
        // рисует, и человек получил бы «создалось что-то невидимое».
        LOG_ERROR("Editor") << "Заготовка интерфейса неизвестна: '" << preset
                            << "' — элемент не создан";
        m_scene->RemoveObject(obj.Id());
        return GameObject{};
    }
    // Новый элемент появляется в центре родителя: у края экрана его легко не
    // заметить и решить, что «ничего не создалось».
    sage::ui::Element& xf = reg.get<sage::ui::Element>(obj.Entity());
    xf.Anchor = UIAnchor::Center;
    xf.Position = glm::vec2(0.0f, 0.0f);

    return obj;
}

GameObject EditorLayer::CreateCubeEntity(const std::string& name) {
    return CreatePrimitiveEntity(name, MeshRef::Type::Cube);
}

GameObject EditorLayer::CreatePrimitiveEntity(const std::string& name, MeshRef::Type type) {
    GameObject obj = m_scene->CreateObject(name);
    MeshRendererComponent& mr = obj.Renderer();
    mr.Ref = MeshRef{type, ""};
    mr.MeshPtr = ResourceManager::Instance().GetPrimitive(type);
    return obj;
}

// --- Создание по каталогу ---------------------------------------------------
//
// ОДНО МЕСТО НА ВСЕ МЕНЮ. Пункты «создать» жили в двух списках сразу — в меню
// «Объект» и под правой кнопкой в иерархии, — и списки эти расходились: под
// правой кнопкой были только «пустой» и «куб». Теперь оба меню рисуют общий
// каталог (editor/src/ObjectCatalog.h) и зовут сюда с его ключом.
//
// ШАБЛОН, А НЕ ГОЛЫЙ КОМПОНЕНТ. Каждый пункт даёт предмет, с которым уже можно
// работать: у падающего ящика есть масса и коллайдер, у прожектора — поворот
// вниз, у персонажа — капсула нужного роста. Пункт, после которого надо
// открыть инспектор и что-то донастроить, чтобы вообще увидеть результат,
// читается как несработавшая кнопка.
int EditorLayer::CreateCatalogObject(const std::string& id) {
    entt::registry& reg = m_scene->Registry();

    // Готовый объект: снимок для отмены уже сделан, осталось выделить и
    // отметить сцену изменённой.
    auto done = [&](GameObject obj) {
        m_selection.SetPrimary(obj.Id());
        m_sceneDirty = true;
        return obj.Id();
    };

    if (id == "empty") {
        PushUndoSnapshot();
        return done(m_scene->CreateEmptyObject("Empty"));
    }

    // --- Формы ---
    struct ShapeEntry { const char* Id; const char* Name; MeshRef::Type Type; };
    static const ShapeEntry kShapes[] = {
        {"shape.cube",     "Cube",     MeshRef::Type::Cube},
        {"shape.sphere",   "Sphere",   MeshRef::Type::Sphere},
        {"shape.plane",    "Plane",    MeshRef::Type::Plane},
        {"shape.cylinder", "Cylinder", MeshRef::Type::Cylinder},
        {"shape.cone",     "Cone",     MeshRef::Type::Cone},
        {"shape.capsule",  "Capsule",  MeshRef::Type::Capsule},
    };
    for (const ShapeEntry& sh : kShapes) {
        if (id != sh.Id) continue;
        PushUndoSnapshot();
        return done(CreatePrimitiveEntity(sh.Name, sh.Type));
    }

    // --- Свет ---
    // Прожектор смотрит ВНИЗ, солнце — наискось: свет, созданный «в никуда»,
    // выглядит как свет, который не работает.
    struct LightEntry {
        const char* Id; const char* Name; LightComponent::Type Kind;
        glm::vec3 Position; glm::vec3 Rotation; glm::vec3 Color; float Intensity;
    };
    static const LightEntry kLights[] = {
        {"light.point", "Light", LightComponent::Type::Point,
         {0.0f, 2.5f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f},
        {"light.spot", "Spotlight", LightComponent::Type::Spot,
         {0.0f, 5.0f, 0.0f}, {-90.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 4.0f},
        {"light.sun", "Sun", LightComponent::Type::Directional,
         {0.0f, 10.0f, 0.0f}, {-55.0f, -25.0f, 0.0f}, {1.0f, 0.95f, 0.85f}, 1.0f},
    };
    for (const LightEntry& l : kLights) {
        if (id != l.Id) continue;
        PushUndoSnapshot();
        // Свет не рисуется мешем — объект пустой: секция «Меш» у лампы
        // предлагала бы ей модель, цвет и тени, которых у света нет.
        GameObject obj = m_scene->CreateEmptyObject(l.Name);
        obj.GetTransform().Position = l.Position;
        obj.GetTransform().Rotation = l.Rotation;
        LightComponent lc;
        lc.Kind = l.Kind;
        lc.Color = l.Color;
        lc.Intensity = l.Intensity;
        reg.emplace<LightComponent>(obj.Entity(), lc);
        return done(obj);
    }

    // --- Камера ---
    if (id == "camera.game") {
        PushUndoSnapshot();
        GameObject obj = m_scene->CreateEmptyObject("Camera");
        // Камера встаёт ТУДА, ОТКУДА СЕЙЧАС СМОТРЯТ. Созданная в начале
        // координат, она чаще всего оказывалась внутри пола, и панель Game
        // показывала темноту — «камера не работает».
        obj.GetTransform().Position = m_camera.Position;
        const glm::vec3 f = m_camera.Front;
        obj.GetTransform().Rotation =
            glm::vec3(glm::degrees(std::asin(glm::clamp(-f.y, -1.0f, 1.0f))),
                      glm::degrees(std::atan2(-f.x, -f.z)), 0.0f);
        reg.emplace<CameraComponent>(obj.Entity());
        return done(obj);
    }

    // --- Физика ---
    if (id == "physics.box") {
        PushUndoSnapshot();
        GameObject obj = CreatePrimitiveEntity("Crate", MeshRef::Type::Cube);
        obj.GetTransform().Position = {0.0f, 4.0f, 0.0f};
        RigidBodyComponent rb;
        rb.Type = sage::physics::BodyType::Dynamic;
        reg.emplace<RigidBodyComponent>(obj.Entity(), rb);
        reg.emplace<ColliderComponent>(obj.Entity());
        return done(obj);
    }
    if (id == "physics.platform") {
        PushUndoSnapshot();
        GameObject obj = CreatePrimitiveEntity("Platform", MeshRef::Type::Cube);
        // Плита, а не куб: половинные размеры коллайдера умножаются на масштаб
        // (см. PhysicsComponents.h), поэтому форма совпадает с видимой без
        // отдельной настройки.
        obj.GetTransform().Scale = {6.0f, 0.5f, 6.0f};
        obj.GetTransform().Position = {0.0f, -0.25f, 0.0f};
        RigidBodyComponent rb;
        rb.Type = sage::physics::BodyType::Static;
        reg.emplace<RigidBodyComponent>(obj.Entity(), rb);
        reg.emplace<ColliderComponent>(obj.Entity());
        return done(obj);
    }
    if (id == "physics.trigger") {
        PushUndoSnapshot();
        // Зона НЕВИДИМА — меша у неё нет вовсе. Гизмо коллайдера редактор
        // рисует и без него, а модель у триггера означала бы, что игрок видит
        // границу, которую видеть не должен.
        GameObject obj = m_scene->CreateEmptyObject("Trigger");
        obj.GetTransform().Position = {0.0f, 1.0f, 0.0f};
        RigidBodyComponent rb;
        rb.Type = sage::physics::BodyType::Static;
        rb.Sensor = true; // пропускает сквозь себя, но сообщает о входе и выходе
        reg.emplace<RigidBodyComponent>(obj.Entity(), rb);
        ColliderComponent col;
        col.HalfExtents = {1.0f, 1.0f, 1.0f};
        reg.emplace<ColliderComponent>(obj.Entity(), col);
        return done(obj);
    }
    if (id == "physics.character") {
        PushUndoSnapshot();
        // КАПСУЛОЙ, а не цилиндром: капсулой персонажа считает и физика (см.
        // ShapeType::Capsule), и видимое тело обязано совпадать с тем, чем он
        // сталкивается, — иначе высоту настраивают в двух местах, сверяя на глаз.
        GameObject obj = CreatePrimitiveEntity("Character", MeshRef::Type::Capsule);
        // МАСШТАБ СЧИТАЕТСЯ ОТ РАЗМЕРА КАПСУЛЫ, а не переписан на глаз.
        //
        // Капсула движка — диаметр 1, высота 2 (те же числа, что у коллайдера;
        // см. BuildCapsule). Контроллер по умолчанию — радиус 0.35 и высота
        // 1.8, то есть 0.7 в ширину. Отсюда масштаб: 0.7 по ширине и 0.9 по
        // высоте.
        //
        // Стояло {0.7, 1.8, 0.7} при капсуле шириной 0.5 — персонаж рисовался
        // 0.35 в ширину, ВДВОЕ ТОНЬШЕ своей физической капсулы. Именно то
        // расхождение, ради устранения которого капсулу и заводили.
        const CharacterControllerComponent shape;
        obj.GetTransform().Scale = {shape.Radius * 2.0f, shape.Height * 0.5f, shape.Radius * 2.0f};
        obj.GetTransform().Position = {0.0f, shape.Height * 0.5f, 0.0f};
        reg.emplace<CharacterControllerComponent>(obj.Entity());
        return done(obj);
    }

    // --- Эффекты ---
    if (id == "fx.decal") {
        PushUndoSnapshot();
        // Наклейка ставится в СЕРЕДИНУ вида и смотрит туда же, куда камера:
        // поставленная в начало координат, она чаще всего оказалась бы внутри
        // пола или далеко за спиной, и первое, что пришлось бы делать, —
        // искать её.
        GameObject d = m_scene->CreateObject("Decal");
        d.Renderer().Ref = MeshRef{MeshRef::Type::None, ""};
        d.GetTransform().Position = m_camera.Position + m_camera.Front * 4.0f;
        // Ось Z наклейки — навстречу камере: проекция идёт вдоль -Z, то есть
        // от зрителя вглубь сцены, как и смотрит человек.
        const glm::vec3 f = -m_camera.Front;
        d.GetTransform().Rotation =
            glm::vec3(glm::degrees(std::asin(glm::clamp(f.y, -1.0f, 1.0f))),
                      glm::degrees(std::atan2(f.x, f.z)), 0.0f);
        d.GetTransform().Scale = glm::vec3(1.0f);
        reg.emplace<DecalComponent>(d.Entity());
        return done(d);
    }
    if (id == "fx.probe") {
        PushUndoSnapshot();
        GameObject obj = m_scene->CreateEmptyObject("Reflection Probe");
        obj.GetTransform().Position = {0.0f, 2.0f, 0.0f};
        reg.emplace<ReflectionProbeComponent>(obj.Entity());
        return done(obj);
    }

    // Частицы: ключ каталога -> название пресета движка. Связь по ИМЕНИ, а не
    // по номеру в реестре: номера сдвинутся от любой вставки нового пресета, и
    // «Огонь» в меню молча стал бы дымом.
    struct FxEntry { const char* Id; const char* Preset; const char* Name; };
    static const FxEntry kFx[] = {
        {"fx.particles.fire",   "Fire",         "Fire"},
        {"fx.particles.smoke",  "Smoke",        "Smoke"},
        {"fx.particles.sparks", "Sparks",       "Sparks"},
        {"fx.particles.splash", "Water Splash", "Water Splash"},
        {"fx.particles.embers", "Embers",       "Embers"},
        {"fx.particles.debris", "Block Break",  "Debris"},
    };
    for (const FxEntry& fx : kFx) {
        if (id != fx.Id) continue;
        const auto& registry = ParticlePresets::Registry();
        int index = 0;
        for (int i = 0; i < (int)registry.size(); ++i) {
            if (std::string(registry[(size_t)i].Name) == fx.Preset) { index = i; break; }
        }
        PushUndoSnapshot();
        GameObject obj = m_scene->CreateEmptyObject(fx.Name);
        obj.GetTransform().Position = {0.0f, 0.5f, 0.0f};
        ParticleEmitterComponent em;
        em.Config = registry[(size_t)index].Make();
        em.Preset = index;
        reg.emplace<ParticleEmitterComponent>(obj.Entity(), em);
        return done(obj);
    }

    // --- Звук и логика ---
    if (id == "audio.source") {
        PushUndoSnapshot();
        GameObject obj = m_scene->CreateEmptyObject("Sound");
        reg.emplace<AudioSourceComponent>(obj.Entity());
        return done(obj);
    }
    if (id == "logic.script") {
        PushUndoSnapshot();
        // Скрипт прикрепляется ПУСТЫМ слотом: подставленный путь к
        // демонстрационному скрипту движка в чужом проекте не существует, и
        // объект приезжал бы уже сломанным.
        GameObject obj = m_scene->CreateEmptyObject("Script Object");
        reg.emplace<ScriptComponent>(obj.Entity());
        return done(obj);
    }

    // --- Анимация ---
    if (id == "anim.model") {
        PushUndoSnapshot();
        // Меш пустой — модель выберут в инспекторе. Без модели анимация
        // показывает встроенный демо-скелет с клипом «Wave», и во вьюпорте
        // сразу видно, что скиннинг работает.
        GameObject obj = m_scene->CreateObject("Animated Model");
        reg.emplace<AnimationComponent>(obj.Entity());
        return done(obj);
    }

    // --- Интерфейс ---
    // Готовые ЭКРАНЫ и готовые ЭЛЕМЕНТЫ. Голый прямоугольник — это ещё не
    // кнопка: чтобы получить её, надо добавить подложку, надпись и реакцию на
    // мышь. Меню отдаёт то, что человек и хотел, сразу собранным.
    if (id.rfind("ui.screen.", 0) == 0) {
        const std::string demo = id.substr(std::string("ui.screen.").size());
        PushUndoSnapshot();
        const int newId = sage::ui::BuildDemo(*m_scene, demo);
        if (newId < 0) return -1;
        m_selection.SetPrimary(newId);
        m_sceneDirty = true;
        // И сразу открывается редактор интерфейса: элемент, которого не видно
        // после создания, выглядит как «кнопка не сработала».
        SetWorkspace(EditorWorkspace::Interface);
        return newId;
    }
    if (id.rfind("ui.", 0) == 0) {
        const std::string preset = id.substr(std::string("ui.").size());
        PushUndoSnapshot();
        GameObject obj = CreateUIEntity(preset);
        SetWorkspace(EditorWorkspace::Interface);
        return done(obj);
    }

    // Неизвестный ключ — это опечатка в каталоге, а не действие пользователя.
    // Молчать нельзя: пункт меню внешне сработал бы и не сделал ничего.
    LOG_WARN("Editor") << "Каталог объектов: неизвестный ключ — " << id;
    return -1;
}

namespace {
// Копирует компонент T с сущности src на copy, если он есть. Дубликат должен
// нести ВСЕ движковые компоненты — раньше копировались только Script/Camera, и
// дубликат света/физического тела/эмиттера молча терял суть оригинала.
template <typename T>
void CopyComponentIfPresent(GameObject& src, GameObject& copy) {
    if (const T* c = src.Registry()->try_get<T>(src.Entity())) {
        copy.Registry()->emplace_or_replace<T>(copy.Entity(), *c);
    }
}
} // namespace

namespace {
// Копирование сущностей и поддеревьев переехало в движок (sage/scene/Prefab.h):
// ровно то же самое нужно игре, а жило оно здесь, в безымянном пространстве
// имён редактора, и потому было недоступно никому, кроме него. Здесь остались
// короткие псевдонимы, чтобы не править два десятка мест вызова.
using sage::scene::CopyAllComponents;
using sage::scene::CopySubtree;
} // namespace

// Копирует одну сущность (без детей) со всеми компонентами; сдвиг, чтобы копия
// не сливалась с оригиналом. Возвращает копию.
GameObject EditorLayer::DuplicateEntity(GameObject src) {
    // ПОДДЕРЕВО, а не одна сущность. Копировались только компоненты выбранного
    // объекта, и его потомки в копию не попадали: Ctrl+D на панели интерфейса
    // давал панель БЕЗ надписи, на двери — дверь без ручки, на составном
    // персонаже — один корень. Заметить это можно было только по тому, что
    // копия «пустая», а причина не видна нигде.
    //
    // Заодно копирование поддерева переписывает ссылки внутрь копии
    // (см. sage::scene::CopySubtree): дубликат двери открывается СВОЕЙ кнопкой.
    GameObject copy = CopySubtree(*m_scene, src.Entity(), *m_scene, entt::null);
    // ИМЯ КОПИИ — КОРОТКОЕ НА ЛЮБОЙ ГЛУБИНЕ. Раньше к имени просто дописывалось
    // « Copy», и копия копии становилась «Куб Copy Copy»: через пять нажатий
    // Ctrl+D имя переставало помещаться в строку списка (см. CopyName.h).
    copy.SetName(sage::scene::CopyName(src.Name(), [this](const std::string& name) {
        return m_scene->FindByName(name).Valid();
    }));
    copy.GetTransform().Position.x += 0.5f;
    return copy;
}

void EditorLayer::DuplicateSelected() {
    if (m_selection.Empty()) return;
    PushUndoSnapshot();
    std::vector<int> copies;
    for (int id : m_selection.All()) {
        GameObject src = m_scene->Get(id);
        if (!src.Valid()) continue;
        entt::entity parent = m_scene->ParentOf(src.Entity()); // копия остаётся у того же родителя
        GameObject copy = DuplicateEntity(src);
        if (parent != entt::null) m_scene->SetParent(copy.Entity(), parent);
        copies.push_back(copy.Id());
    }
    m_selection.Set(copies);
}

namespace {
// Сколько сущностей под этой в иерархии. Нужно ровно для одного: сказать
// человеку в вопросе, СКОЛЬКО он на самом деле удаляет.
int CountDescendants(Scene& scene, entt::entity e) {
    const HierarchyComponent* h = scene.Registry().try_get<HierarchyComponent>(e);
    if (!h) return 0;
    int n = 0;
    for (entt::entity kid : h->Children) {
        if (!scene.Registry().valid(kid)) continue;
        n += 1 + CountDescendants(scene, kid);
    }
    return n;
}
} // namespace

void EditorLayer::DeleteSelected() {
    int count = 0;
    std::string firstName;
    for (int id : m_selection.All()) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        if (count == 0) firstName = o.Name();
        ++count;
    }
    if (count == 0) return;

    // Спрашиваем — и считаем ПОДДЕРЕВО, а не только выделенное: удаление
    // родителя уносит детей, и человек, выделивший одну строку в иерархии,
    // сплошь и рядом не помнит, сколько под ней.
    int withChildren = 0;
    for (int id : m_selection.All()) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        withChildren += 1 + CountDescendants(*m_scene, o.Entity());
    }

    std::string message;
    if (count == 1) {
        message = T("Delete \u00ab") + firstName + "»?";
        if (withChildren > 1)
            message += T("\nTogether with its children that is ") + std::to_string(withChildren) + T(" entities.");
    } else {
        message = T("Delete the selected objects (") + std::to_string(count) + ")?";
        if (withChildren > count)
            message += T("\nTogether with its children that is ") + std::to_string(withChildren) + T(" entities.");
    }
    message += T("\nCtrl+Z undoes this.");

    m_confirm.Ask("delete-entity", T("Deleting an object"), message, [this]() {
        PushUndoSnapshot();
        for (int id : m_selection.All())
            if (m_scene->Get(id).Valid()) m_scene->RemoveObject(id); // удаляет и поддерево
        m_selection.Clear();
    });
}

// Замок запоминает ТО, ЧТО ПОКАЗАНО СЕЙЧАС, в момент запирания — и держит,
// пока его не откроют. Хранится идентификатор, а не GameObject: за время под
// замком сцену могли перезагрузить (откат, открытие другой), и объект по
// указателю оказался бы чужим.
// Правила выделения и замка живут в EditorSelection — здесь остаётся только
// связь со сценой и панелью ассетов, про которые выделение не знает.
void EditorLayer::SetInspectorLocked(bool locked) {
    if (locked) m_selection.Lock(m_selection.Primary(), m_assets.Selected());
    else m_selection.Unlock();
}

GameObject EditorLayer::InspectedObject() {
    if (!m_selection.Locked()) return SelectedObject();
    // Запертый объект мог исчезнуть — сцену перезагрузили или его удалили.
    // Тогда панель честно пуста, а не показывает чужие поля по старому номеру.
    return m_scene ? m_scene->Get(m_selection.LockedEntityId()) : GameObject{};
}

// ============================================================================
//  Префабы — переиспользуемые сущности-поддеревья (.sageprefab). Формат —
//  та же JSON-сериализация, что у сцен: префаб = мини-сцена с одним корнем.
// ============================================================================
bool EditorLayer::SaveSelectedAsPrefab(const fs::path& path, std::string& err) {
    GameObject root = m_scene->Get(m_selection.Primary());
    if (!root.Valid()) { err = T("nothing selected"); return false; }
    if (!sage::scene::SavePrefab(*m_scene, root.Entity(), path.string(), err)) return false;
    SetStatusMessage(T("Prefab saved: ") + path.filename().string());
    return true;
}

int EditorLayer::InstantiatePrefab(const fs::path& path) {
    PushUndoSnapshot();
    const int rootId = sage::scene::InstantiatePrefab(*m_scene, path.string());
    if (rootId != -1) m_selection.SetPrimary(rootId);
    return rootId;
}

// ============================================================================
//  Инструменты над выделением
// ============================================================================

namespace {

// Мировой AABB одной сущности: восемь углов локальной коробки через мировую
// матрицу. Не «центр ± радиус»: при повороте коробка перестаёт быть выровненной
// по осям, и охватывающая её мировая коробка строится только по углам.
bool EntityWorldBounds(Scene& scene, entt::entity e, glm::vec3& lo, glm::vec3& hi) {
    const MeshRendererComponent* mr = scene.Registry().try_get<MeshRendererComponent>(e);
    if (!mr || !mr->MeshPtr) return false;
    const glm::vec3 bmin = mr->MeshPtr->BoundsMin();
    const glm::vec3 bmax = mr->MeshPtr->BoundsMax();
    const glm::mat4 world = scene.WorldMatrix(e);
    bool any = false;
    for (int c = 0; c < 8; ++c) {
        const glm::vec3 corner((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y,
                               (c & 4) ? bmax.z : bmin.z);
        const glm::vec3 w = glm::vec3(world * glm::vec4(corner, 1.0f));
        lo = any ? glm::min(lo, w) : w;
        hi = any ? glm::max(hi, w) : w;
        any = true;
    }
    return any;
}

} // namespace

bool EditorLayer::SelectionBounds(glm::vec3& outMin, glm::vec3& outMax) {
    bool any = false;
    for (int id : m_selection.All()) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        glm::vec3 lo, hi;
        if (!EntityWorldBounds(*m_scene, o.Entity(), lo, hi)) continue;
        outMin = any ? glm::min(outMin, lo) : lo;
        outMax = any ? glm::max(outMax, hi) : hi;
        any = true;
    }
    return any;
}

void EditorLayer::FocusSelected() {
    glm::vec3 lo, hi;
    glm::vec3 target;
    float radius = 1.0f;
    if (SelectionBounds(lo, hi)) {
        target = (lo + hi) * 0.5f;
        radius = std::max(glm::length(hi - lo) * 0.5f, 0.1f);
    } else {
        // Выделено что-то без геометрии (свет, камера, пустышка) — подводим
        // камеру к его позиции: маркер всё равно нарисован, и добраться до него
        // человек хочет ровно так же.
        GameObject o = SelectedObject();
        if (!o.Valid()) return;
        target = glm::vec3(m_scene->WorldMatrix(o.Entity())[3]);
    }

    // Расстояние — из вертикального угла обзора, чтобы объект занял кадр
    // примерно на 70%: впритык он упирался бы в края, а «с запасом» съедало бы
    // смысл операции.
    const float fov = glm::radians(std::max(m_camera.Fov, 10.0f));
    const float dist = std::max(radius / std::tan(fov * 0.5f) / 0.7f, radius + 0.5f);
    m_camera.Position = target - m_camera.Front * dist;
}

void EditorLayer::DropSelectedToSurface() {
    if (m_selection.Empty()) return;
    PushUndoSnapshot();

    int moved = 0;
    for (int id : m_selection.All()) {
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        glm::vec3 lo, hi;
        if (!EntityWorldBounds(*m_scene, o.Entity(), lo, hi)) continue;

        // Луч вниз из центра НИЖНЕЙ грани: из центра объекта он сначала прошёл
        // бы сквозь него самого, а из угла — промахнулся бы мимо опоры.
        const glm::vec3 bottom((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f);
        const glm::vec3 ro = bottom + glm::vec3(0.0f, 0.001f, 0.0f);
        const glm::vec3 rd(0.0f, -1.0f, 0.0f);

        float bestT = 1e30f;
        bool found = false;
        auto view = m_scene->Registry().view<IdComponent, MeshRendererComponent>();
        for (auto e : view) {
            // Себя и других выделенных пропускаем: они едут вместе с этим, и
            // опираться на них значило бы ставить объект сам на себя.
            if (m_selection.Contains(view.get<IdComponent>(e).Id)) continue;
            Mesh* mesh = view.get<MeshRendererComponent>(e).MeshPtr.get();
            if (!mesh) continue;
            const glm::mat4 inv = glm::inverse(m_scene->WorldMatrix(e));
            const glm::vec3 lro = glm::vec3(inv * glm::vec4(ro, 1.0f));
            const glm::vec3 lrd = glm::vec3(inv * glm::vec4(rd, 0.0f));
            sage::render::RayHit hit = sage::render::RayMesh(*mesh, lro, lrd);
            if (hit.Hit && hit.Distance < bestT) {
                bestT = hit.Distance;
                found = true;
            }
        }
        if (!found) continue;

        // Двигаем на дельту в МИРЕ, а потом переводим в локальные координаты:
        // у сущности с родителем прибавление к Transform.Position означало бы
        // смещение в системе родителя, то есть не туда.
        const float dropBy = bestT;
        Transform& tr = o.GetTransform();
        const entt::entity parent = m_scene->ParentOf(o.Entity());
        if (parent == entt::null) {
            tr.Position.y -= dropBy;
        } else {
            glm::mat4 world = m_scene->WorldMatrix(o.Entity());
            world[3].y -= dropBy;
            const glm::mat4 local = glm::inverse(m_scene->WorldMatrix(parent)) * world;
            tr.Position = glm::vec3(local[3]);
        }
        ++moved;
    }
    SetStatusMessage(moved ? (T("Dropped onto the surface: ") + std::to_string(moved))
                           : T("There is no surface under the selection"));
}

void EditorLayer::AlignSelection(int axis) {
    if (m_selection.All().size() < 2 || axis < 0 || axis > 2) return;
    GameObject primary = SelectedObject();
    if (!primary.Valid()) return;
    PushUndoSnapshot();

    // Эталон — первичная сущность (та, вокруг которой стоит гизмо): выравнивать
    // «по среднему» бессмысленно, человек всегда равняет ПО ЧЕМУ-ТО.
    const float target = m_scene->WorldMatrix(primary.Entity())[3][axis];
    for (int id : m_selection.All()) {
        if (id == primary.Id()) continue;
        GameObject o = m_scene->Get(id);
        if (!o.Valid()) continue;
        glm::mat4 world = m_scene->WorldMatrix(o.Entity());
        world[3][axis] = target;
        const entt::entity parent = m_scene->ParentOf(o.Entity());
        const glm::mat4 local =
            (parent == entt::null) ? world : glm::inverse(m_scene->WorldMatrix(parent)) * world;
        o.GetTransform().Position = glm::vec3(local[3]);
    }
    SetStatusMessage(std::string(T("Aligned along axis ")) + "XYZ"[axis]);
}

bool EditorLayer::HasPrimaryCamera() {
    auto view = m_scene->Registry().view<CameraComponent, Transform>();
    for (auto e : view) {
        if (view.get<CameraComponent>(e).Primary) return true;
    }
    return false;
}

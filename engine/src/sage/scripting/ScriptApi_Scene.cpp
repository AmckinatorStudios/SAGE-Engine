#include "ScriptEngine.h"
#include "ScriptApiCommon.h"

#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/scene/Prefab.h"
#include "sage/ui/UISceneSystem.h"

// ---------------------------------------------------------------------------
// Сцена и объекты: GameObject, sage.scene.*, sage.render.* (меши и материалы)
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterGameObject() {
    // GameObject — то, что скрипт получает как "entity" в OnUpdate/OnStart,
    // или что возвращают SpawnObject/FindObject. Теперь это дескриптор поверх
    // ECS (см. Scene.h), поэтому поля выставлены через property-аксессоры к
    // компонентам сущности. Transform/Color отдаются ССЫЛКОЙ — чтобы прежний
    // код вида `entity.Transform.Rotation.y = ...` и `cube.Color.x = ...`
    // писал прямо в компонент; для Color есть и сеттер целого значения
    // (`cube.Color = Vec3.new(...)`), чтобы поведение совпало со старым
    // data-member-биндингом один в один.
    sol::usertype<GameObject> goType = m_lua.new_usertype<GameObject>("GameObject",
        "Id", sol::property([](GameObject& o) { return o.Id(); }),
        "Name", sol::property(
            [](GameObject& o) { return o.Name(); },
            [](GameObject& o, const std::string& n) { o.SetName(n); }),
        "Transform", sol::property([](GameObject& o) -> ::Transform& { return o.GetTransform(); }),
        "Color", sol::property(
            [](GameObject& o) -> glm::vec3& { return o.ColorRef(); },
            [](GameObject& o, const glm::vec3& c) { o.ColorRef() = c; }),
        // Свечение — ровно как Color: ссылкой (можно писать по компонентам) и
        // целым значением. Живёт оно в рендерере, но для скрипта это свойство
        // объекта: строка `lamp.Emissive = Vec3(...)` рядом с `lamp.Color =
        // Vec3(...)` не должна вдруг требовать другого способа обращения.
        "Emissive", sol::property(
            [](GameObject& o) -> glm::vec3& { return o.EmissiveRef(); },
            [](GameObject& o, const glm::vec3& c) { o.EmissiveRef() = c; }),
        "EmissiveStrength", sol::property(
            [](GameObject& o) { return o.EmissiveStrengthRef(); },
            [](GameObject& o, float v) { o.EmissiveStrengthRef() = v; }),
        "Valid", [](GameObject& o) { return o.Valid(); }
    );

    // Аксессоры компонентов (Has/Get/Add/Remove) — единый шаблон на каждый тип.
    sage::scripting::detail::BindComponentAccessors<LightComponent>(goType, "HasLight", "GetLight", "AddLight", "RemoveLight");
    sage::scripting::detail::BindComponentAccessors<CameraComponent>(goType, "HasCamera", "GetCamera", "AddCamera", "RemoveCamera");
    sage::scripting::detail::BindComponentAccessors<RigidBodyComponent>(goType, "HasRigidBody", "GetRigidBody", "AddRigidBody", "RemoveRigidBody");
    sage::scripting::detail::BindComponentAccessors<ColliderComponent>(goType, "HasCollider", "GetCollider", "AddCollider", "RemoveCollider");
    sage::scripting::detail::BindComponentAccessors<JointComponent>(goType, "HasJoint", "GetJoint", "AddJoint", "RemoveJoint");
    sage::scripting::detail::BindComponentAccessors<ParticleEmitterComponent>(goType, "HasEmitter", "GetEmitter", "AddEmitter", "RemoveEmitter");
    sage::scripting::detail::BindComponentAccessors<MeshRendererComponent>(goType, "HasRenderer", "GetRenderer", "AddRenderer", "RemoveRenderer");
    sage::scripting::detail::BindComponentAccessors<ScriptComponent>(goType, "HasScript", "GetScript", "AddScript", "RemoveScript");
    sage::scripting::detail::BindComponentAccessors<UIElementComponent>(goType, "HasUI", "GetUI", "AddUI", "RemoveUI");
    sage::scripting::detail::BindComponentAccessors<DecalComponent>(goType, "HasDecal", "GetDecal", "AddDecal", "RemoveDecal");

    // --- Иерархия прямо на объекте: e:SetParent(p) / e:Parent() / e:Children() /
    // e:WorldPosition() / e:Destroy(). Всё маршрутизируется через Scene (циклы
    // предотвращаются, мировая матрица считается по цепочке родителей). ---
    goType["SetParent"] = [this](GameObject& child, GameObject& parent) {
        if (!m_scene) throw std::runtime_error("SetParent: сцена не привязана (BindScene не вызван)");
        if (child.Valid())
            m_scene->SetParent(child.Entity(), parent.Valid() ? parent.Entity() : entt::null);
    };
    goType["Unparent"] = [this](GameObject& child) {
        if (m_scene && child.Valid()) m_scene->SetParent(child.Entity(), entt::null);
    };
    goType["Parent"] = [this](GameObject& o) -> sol::optional<GameObject> {
        if (!m_scene || !o.Valid()) return sol::nullopt;
        entt::entity p = m_scene->ParentOf(o.Entity());
        if (p == entt::null) return sol::nullopt;
        return GameObject(&m_scene->Registry(), p);
    };
    goType["Children"] = [this](GameObject& o) -> sol::table {
        sol::table list = m_lua.create_table();
        if (m_scene && o.Valid()) {
            if (auto* h = m_scene->Registry().try_get<HierarchyComponent>(o.Entity())) {
                int i = 1;
                for (auto c : h->Children)
                    if (m_scene->Registry().valid(c)) list[i++] = GameObject(&m_scene->Registry(), c);
            }
        }
        return list;
    };
    goType["WorldPosition"] = [this](GameObject& o) -> glm::vec3 {
        if (!m_scene || !o.Valid()) return glm::vec3(0.0f);
        return glm::vec3(m_scene->WorldMatrix(o.Entity())[3]);
    };
    goType["Destroy"] = [this](GameObject& o) {
        if (m_scene && o.Valid()) m_scene->RemoveObject(o.Id());
    };
}

void ScriptEngine::RegisterSceneApi() {
    // Простая функция логирования, чтобы скрипты могли печатать отладочную информацию
    Bind("core", "log", "log", [](const std::string& message) {
        LOG_INFO("Lua") << message;
    });

    // --- Сцена: спавн/поиск/удаление объектов из Lua (доступно после BindScene) ---
    Bind("scene", "Spawn", "SpawnObject", [this](const std::string& name) -> GameObject {
        if (!m_scene) throw std::runtime_error("SpawnObject: сцена не привязана (ScriptEngine::BindScene не вызван)");
        GameObject obj = m_scene->CreateObject(name);
        // Уровень Trace, а не Info, НАМЕРЕННО. Скрипт, порождающий мир (воксельный
        // ландшафт — тысячи блоков за кадр загрузки), давал по строке лога на
        // каждый блок: консоль редактора забивалась, а сам вывод стоил дороже
        // создания сущности. Разовые спавны по-прежнему видны — достаточно
        // опустить порог лога (см. Log.h).
        LOG_TRACE("Lua") << "Создан объект: " << name << " (id " << obj.Id() << ")";
        return obj;
    });

    Bind("scene", "Find", "FindObject", [this](const std::string& name) -> sol::optional<GameObject> {
        if (!m_scene) return sol::nullopt;
        GameObject obj = m_scene->FindByName(name);
        if (!obj.Valid()) return sol::nullopt;
        return obj;
    });

    // Верхний видимый UI-элемент под экранной точкой (учитывает слои/маски):
    // GameObject или nil. Экранный размер передаётся явно — скрипт берёт его
    // из своего контекста (окно игры / панель Game).
    Bind("ui", "ElementAt", "GetUIElementAt", [this](float x, float y, int screenW, int screenH) -> sol::optional<GameObject> {
            if (!m_scene) return sol::nullopt;
            int id = sage::ui::HitTest(*m_scene, x, y, screenW, screenH);
            if (id < 0) return sol::nullopt;
            GameObject obj = m_scene->Get(id);
            if (!obj.Valid()) return sol::nullopt;
            return obj;
        });

    Bind("scene", "Destroy", "DestroyObject", [this](int id) {
        if (!m_scene) throw std::runtime_error("DestroyObject: сцена не привязана (ScriptEngine::BindScene не вызван)");
        // RemoveObject уничтожает сущность в ECS-registry. Зависимые
        // ScriptInstance держат дескриптор {registry, entity}, а не сырой
        // указатель, поэтому после уничтожения их Object.Valid() станет false —
        // UpdateAll() безопасно пропустит и уберёт их после текущего прохода
        // (никакого use-after-free, даже если скрипт уничтожает сам себя).
        m_scene->RemoveObject(id);
    });

    // --- Префабы: заготовленный объект в мир --------------------------------
    //
    // Формально префабы были и раньше — но только в редакторе, внутри его
    // безымянного пространства имён. Для игры про постройку из блоков это
    // значит, что главной её операции не существовало: поставить готовый
    // объект в мир из скрипта было нечем, и приходилось каждый раз собирать
    // его по компонентам заново.
    //
    // Разобранный префаб кэшируется, поэтому тысяча блоков — это один разбор
    // JSON, а не тысяча.
    Bind("scene", "SpawnPrefab", "SpawnPrefab",
         [this](const std::string& path, sol::optional<glm::vec3> at) -> int {
             if (!m_scene) throw std::runtime_error("SpawnPrefab: сцена не привязана");
             const std::string file = sage::AssetDatabase::Instance().LocatePath(path);
             return at ? sage::scene::InstantiatePrefabAt(*m_scene, file, *at)
                       : sage::scene::InstantiatePrefab(*m_scene, file);
         });

    // Сохранить сущность (с потомками) как шаблон прямо из игры. Нужно не для
    // симметрии: редактор карт, сделанный НА движке, — обычная часть игры про
    // постройку, и без записи префабов он невозможен.
    Bind("scene", "SavePrefab", "SavePrefab",
         [this](int id, const std::string& path) -> bool {
             if (!m_scene) throw std::runtime_error("SavePrefab: сцена не привязана");
             GameObject obj = m_scene->Get(id);
             if (!obj.Valid()) return false;
             std::string err;
             if (sage::scene::SavePrefab(*m_scene, obj.Entity(), path, err)) return true;
             LOG_ERROR("Prefab") << "Не удалось сохранить префаб " << path << ": " << err;
             return false;
         });
}

void ScriptEngine::RegisterMeshApi() {
    // Удобный хелпер: даёт заспавненному объекту видимый куб-меш, чтобы его
    // сразу было видно на сцене без ручной возни с MeshRef/ResourceManager
    Bind("render", "SetMeshCube", "SetMeshCube", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
        mr.MeshPtr = ResourceManager::Instance().GetCube();
    });

    // То же самое, но грузит .obj модель (кэшируется ResourceManager'ом —
    // одна и та же модель, запрошенная из нескольких скриптов, не
    // перечитывается с диска). Бросает ошибку, если файл не найден/битый.
    Bind("render", "SetMeshModel", "SetMeshModel", [](GameObject& obj, const std::string& path) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Model, path};
        mr.MeshPtr = ResourceManager::Instance().GetModel(path);
        // GetModel при ошибке возвращает nullptr (чтобы загрузка СЦЕНЫ с битой
        // моделью не обрывалась) — но скрипту отдаём прежний контракт: ошибка.
        if (!mr.MeshPtr) throw std::runtime_error("SetMeshModel: модель не загрузилась: " + path);
    });

    // Остальные встроенные примитивы — как SetMeshCube, но для сферы/плоскости/
    // цилиндра/конуса. SetMeshNone убирает геометрию (пустышка/маркер/держатель
    // компонентов).
    Bind("render", "SetMeshSphere", "SetMeshSphere", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Sphere, ""};
        mr.MeshPtr = ResourceManager::Instance().GetSphere();
    });
    Bind("render", "SetMeshPlane", "SetMeshPlane", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Plane, ""};
        mr.MeshPtr = ResourceManager::Instance().GetPlane();
    });
    Bind("render", "SetMeshCylinder", "SetMeshCylinder", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cylinder, ""};
        mr.MeshPtr = ResourceManager::Instance().GetCylinder();
    });
    Bind("render", "SetMeshCone", "SetMeshCone", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cone, ""};
        mr.MeshPtr = ResourceManager::Instance().GetCone();
    });
    // Прозрачность объекта одним вызовом: не заставлять скрипт доставать
    // компонент ради одного числа, которое меняется чаще всего остального
    // (затухание подобранного предмета, вода, призрачная подсветка постройки).
    Bind("render", "SetOpacity", "SetOpacity", [](GameObject& obj, float opacity) {
        obj.Renderer().Opacity = opacity;
    });
    Bind("render", "GetOpacity", "GetOpacity", [](GameObject& obj) {
        return obj.Renderer().Opacity;
    });

    // Юниформы собственного шейдера ДЛЯ ОДНОЙ сущности: материал общий, а
    // «подсвети именно эту доску» — задача штучная. Тип выводится из значения:
    // число -> float, Vec2/Vec3/Vec4 -> соответствующий вектор.
    Bind("render", "SetShaderParam", "SetShaderParam", [](GameObject& obj, const std::string& name,
                                            sol::object value) {
        if (!obj.Valid()) return;
        auto& comp = obj.Registry()->get_or_emplace<ShaderParamsComponent>(obj.Entity());
        if (value.is<float>())            comp.Params[name] = ShaderParam::Make(value.as<float>());
        else if (value.is<glm::vec2>())   comp.Params[name] = ShaderParam::Make(value.as<glm::vec2>());
        else if (value.is<glm::vec3>())   comp.Params[name] = ShaderParam::Make(value.as<glm::vec3>());
        else if (value.is<glm::vec4>())   comp.Params[name] = ShaderParam::Make(value.as<glm::vec4>());
        else throw std::runtime_error("SetShaderParam: значение должно быть числом или Vec2/3/4");
    });
    Bind("render", "ClearShaderParams", "ClearShaderParams", [](GameObject& obj) {
        if (obj.Valid()) obj.Registry()->remove<ShaderParamsComponent>(obj.Entity());
    });

    // Материал сущности: путь к .sagemat. Пусто — снять материал и вернуться к
    // собственному цвету. Через него игра и назначает свой шейдер.
    Bind("render", "SetMaterial", "SetMaterial", [](GameObject& obj, const std::string& path) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.MaterialPath = path;
        mr.MaterialPtr = path.empty() ? nullptr : ResourceManager::Instance().GetMaterial(path);
    });

    // Юниформа МАТЕРИАЛА — общая для всех, кто им покрашен. Так вода правит
    // одно значение на тысячу тайлов, не разбивая их инстансную группу
    // (штучный SetShaderParam разбил бы). Материал берётся из общего кэша,
    // поэтому правка видна всем сущностям с этим путём немедленно.
    Bind("render", "SetMaterialParam", "SetMaterialParam", [](const std::string& path, const std::string& name,
                                              sol::object value) {
        std::shared_ptr<Material> mat = ResourceManager::Instance().GetMaterial(path);
        if (!mat) throw std::runtime_error("SetMaterialParam: материал не найден: " + path);
        if (value.is<float>())            mat->Params[name] = ShaderParam::Make(value.as<float>());
        else if (value.is<glm::vec2>())   mat->Params[name] = ShaderParam::Make(value.as<glm::vec2>());
        else if (value.is<glm::vec3>())   mat->Params[name] = ShaderParam::Make(value.as<glm::vec3>());
        else if (value.is<glm::vec4>())   mat->Params[name] = ShaderParam::Make(value.as<glm::vec4>());
        else throw std::runtime_error("SetMaterialParam: значение должно быть числом или Vec2/3/4");
    });

    Bind("render", "SetMeshNone", "SetMeshNone", [](GameObject& obj) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::None, ""};
        mr.MeshPtr.reset();
    });
}


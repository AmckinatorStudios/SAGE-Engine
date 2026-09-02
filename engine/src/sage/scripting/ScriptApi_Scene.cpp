#include "ScriptEngine.h"
#include "ScriptApiCommon.h"

#include "sage/core/Config.h"
#include "sage/core/Log.h"
#include "sage/render/DebugView.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/TextureGen.h"
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
    BindUIAccessors(goType);
    sage::scripting::detail::BindComponentAccessors<DecalComponent>(goType, "HasDecal", "GetDecal", "AddDecal", "RemoveDecal");

    // --- Иерархия прямо на объекте: e:SetParent(p) / e:Parent() / e:Children() /
    // e:WorldPosition() / e:Destroy(). Всё маршрутизируется через Scene (циклы
    // предотвращаются, мировая матрица считается по цепочке родителей). ---
    // РАБОТА С УНИЧТОЖЕННОЙ СУЩНОСТЬЮ — ОШИБКА, А НЕ ТИХИЙ ОТВЕТ.
    //
    // Самая частая ошибка живого игрового кода: скрипт запомнил врага, враг
    // умер, скрипт на следующем кадре трогает его. Поля сущности (e.Transform,
    // e.Name) об этом сообщали всегда — движок бросает исключение, sol2
    // превращает его в ошибку Lua с именем файла и строкой. А методы иерархии
    // молчали: WorldPosition() мёртвой сущности возвращал (0,0,0), SetParent
    // ничего не делал. Ноль здесь хуже ошибки: он выглядит как настоящая
    // координата, и объект уезжает в начало мира — «предметы иногда
    // телепортируются в центр карты», ищи потом причину.
    //
    // Единственное исключение — Destroy(): удалить уже удалённое не ошибка, а
    // обычная ситуация двух скриптов, целящихся в одного врага.
    auto requireAlive = [](const GameObject& o, const char* who) {
        if (!o.Valid()) throw std::runtime_error(std::string(who) + ": сущность уже уничтожена");
    };
    goType["SetParent"] = [this, requireAlive](GameObject& child, GameObject& parent) {
        if (!m_scene) throw std::runtime_error("SetParent: сцена не привязана (BindScene не вызван)");
        requireAlive(child, "SetParent");
        m_scene->SetParent(child.Entity(), parent.Valid() ? parent.Entity() : entt::null);
    };
    goType["Unparent"] = [this, requireAlive](GameObject& child) {
        if (!m_scene) throw std::runtime_error("Unparent: сцена не привязана (BindScene не вызван)");
        requireAlive(child, "Unparent");
        m_scene->SetParent(child.Entity(), entt::null);
    };
    goType["Parent"] = [this, requireAlive](GameObject& o) -> sol::optional<GameObject> {
        if (!m_scene) throw std::runtime_error("Parent: сцена не привязана (BindScene не вызван)");
        requireAlive(o, "Parent");
        entt::entity p = m_scene->ParentOf(o.Entity());
        if (p == entt::null) return sol::nullopt; // нет родителя — законный ответ
        return GameObject(&m_scene->Registry(), p);
    };
    goType["Children"] = [this, requireAlive](GameObject& o) -> sol::table {
        if (!m_scene) throw std::runtime_error("Children: сцена не привязана (BindScene не вызван)");
        requireAlive(o, "Children");
        sol::table list = m_lua.create_table();
        if (auto* h = m_scene->Registry().try_get<HierarchyComponent>(o.Entity())) {
            int i = 1;
            for (auto c : h->Children)
                if (m_scene->Registry().valid(c)) list[i++] = GameObject(&m_scene->Registry(), c);
        }
        return list; // пустой список — законный ответ: детей нет
    };
    goType["WorldPosition"] = [this, requireAlive](GameObject& o) -> glm::vec3 {
        if (!m_scene) throw std::runtime_error("WorldPosition: сцена не привязана (BindScene не вызван)");
        requireAlive(o, "WorldPosition");
        return glm::vec3(m_scene->WorldMatrix(o.Entity())[3]);
    };
    goType["Destroy"] = [this](GameObject& o) {
        if (!m_scene) throw std::runtime_error("Destroy: сцена не привязана (BindScene не вызван)");
        if (o.Valid()) m_scene->RemoveObject(o.Id()); // повторное удаление — не ошибка
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
        // Сцены НЕТ — это не «объект не найден», а не настроенный хозяин кадра
        // (забыли BindScene). Разница существенная: nil здесь читается скриптом
        // как «такого объекта в сцене нет», и он спокойно идёт дальше по ветке
        // «ну и ладно» — а на самом деле для него нет вообще ничего. Соседние
        // SpawnObject/DestroyObject об этом честно сообщают, и обещание в
        // заголовке ScriptEngine.h ровно такое же.
        if (!m_scene) throw std::runtime_error("FindObject: сцена не привязана (ScriptEngine::BindScene не вызван)");
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

    // Принимает СУЩНОСТЬ ИЛИ ЕЁ НОМЕР, как и SendMessage.
    //
    // Раньше здесь стоял только `int id`, и это была тихая ловушка. Самый
    // естественный код на Lua —
    //
    //     local v = FindObject("Victim")
    //     DestroyObject(v)
    //
    // — не удалял НИЧЕГО и не сообщал об этом: FindObject отдаёт GameObject,
    // а sol2 без включённых проверок превращал userdata в число нулём. То есть
    // движок исправно удалял сущность с номером 0, которой не бывает. Ошибка
    // при этом выглядела как «удаление не работает через раз»: через
    // entity:Destroy() всё получалось, через DestroyObject(entity) — нет.
    Bind("scene", "Destroy", "DestroyObject", [this](sol::object target) {
        if (!m_scene) throw std::runtime_error("DestroyObject: сцена не привязана (ScriptEngine::BindScene не вызван)");
        int id = -1;
        if (target.is<GameObject>()) {
            GameObject o = target.as<GameObject>();
            if (!o.Valid()) return; // уже удалён — не ошибка: в игре так бывает
            id = o.Id();
        } else if (target.is<int>()) {
            id = target.as<int>();
        } else {
            throw std::runtime_error("DestroyObject: ожидается сущность или её номер");
        }
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

    // --- Процедурные текстуры (см. render/TextureGen.h) ----------------------
    //
    // Скрипт описывает картинку таблицей и получает ИМЯ, под которым она легла
    // в кэш ресурсов; дальше это обычная текстура — её ставят материалу
    // (m.TexturePath = имя), она участвует в мипмапах и фильтрации, её видит
    // редактор.
    //
    //   local id = sage.texture.Generate("floor", {
    //       pattern = "checker", width = 1024, tilesX = 8, tilesY = 8,
    //       colorA = Vec3(0.82, 0.82, 0.84), colorB = Vec3(0.30, 0.31, 0.34),
    //   })
    //
    // Повторный вызов с тем же именем ПЕРЕСЧИТЫВАЕТ картинку: ровно затем её и
    // зовут второй раз — поменять размер клетки или цвет.
    Bind("texture", "Generate", "GenerateTexture",
         [](const std::string& name, sol::table t) -> std::string {
             if (name.empty()) throw std::runtime_error("texture.Generate: имя не может быть пустым");
             sage::render::TextureRecipe r;
             const std::string pattern = t.get_or("pattern", std::string("checker"));
             if (!sage::render::ParseTexturePattern(pattern, r.Kind)) {
                 throw std::runtime_error("texture.Generate: неизвестный узор '" + pattern +
                                          "' (solid/checker/grid/noise/gradient/bricks/dots)");
             }
             r.Width = t.get_or("width", r.Width);
             r.Height = t.get_or("height", r.Width);   // квадрат по умолчанию
             if (sol::optional<glm::vec3> a = t["colorA"]) r.ColorA = glm::vec4(*a, 1.0f);
             if (sol::optional<glm::vec3> b = t["colorB"]) r.ColorB = glm::vec4(*b, 1.0f);
             if (sol::optional<glm::vec4> a = t["colorA"]) r.ColorA = *a;
             if (sol::optional<glm::vec4> b = t["colorB"]) r.ColorB = *b;
             r.TilesX = t.get_or("tilesX", r.TilesX);
             r.TilesY = t.get_or("tilesY", r.TilesX);
             r.LineWidth = t.get_or("lineWidth", r.LineWidth);
             r.Offset = t.get_or("offset", r.Offset);
             r.Angle = t.get_or("angle", r.Angle);
             r.Radial = t.get_or("radial", r.Radial);
             r.Octaves = t.get_or("octaves", r.Octaves);
             r.Frequency = t.get_or("frequency", r.Frequency);
             r.Persistence = t.get_or("persistence", r.Persistence);
             r.Seed = (unsigned int)t.get_or("seed", (int)r.Seed);
             r.Grain = t.get_or("grain", r.Grain);
             r.AsNormal = t.get_or("normal", r.AsNormal);
             r.NormalStrength = t.get_or("strength", r.NormalStrength);

             // Анизотропия по умолчанию: пол и стены видны под острым углом
             // чаще всего остального, и без неё дальняя половина пола
             // превращается в мыло — то есть ровно там, где текстура и нужна.
             std::shared_ptr<Texture> tex =
                 sage::render::GenerateTexture(r, TextureFilter::Anisotropic, true);
             ResourceManager::Instance().RegisterTexture(name, tex);
             return name;
         });

    // --- Отладочный вид кадра ------------------------------------------------
    //
    // Игра включает разбор кадра по слагаемым (нормали, шероховатость, тени,
    // каскады — см. render/DebugView.h) сама. Раньше это умели только
    // настройки и переменная окружения, то есть посмотреть на нормали можно
    // было, лишь перезапустив игру, — а нужно это ровно наоборот: не отходя от
    // того места, где картинка выглядит странно.
    //
    // Возвращает false на незнакомое имя, а не молча включает обычный вид:
    // иначе опечатка выглядела бы как «режим не работает».
    Bind("render", "SetDebugView", "SetDebugView", [](const std::string& name) -> bool {
        sage::render::DebugView view = sage::render::DebugView::None;
        if (!sage::render::ParseDebugView(name.c_str(), view)) return false;
        sage::EngineConfig::Get().DebugView = name;
        return true;
    });
    Bind("render", "GetDebugView", "GetDebugView", []() -> std::string {
        return sage::EngineConfig::Get().DebugView;
    });
    // Список видов: имя для настроек, подпись человеку и что в нём смотреть.
    // Скрипту он нужен, чтобы перебирать виды по клавише, не заводя свою копию
    // списка — которая разойдётся с движком на первом же новом виде.
    Bind("render", "DebugViews", "DebugViews", [this]() {
        sol::table list = m_lua.create_table();
        for (std::size_t i = 0; i < sage::render::DebugViewCount(); ++i) {
            const auto view = (sage::render::DebugView)i;
            sol::table item = m_lua.create_table();
            item["id"] = sage::render::DebugViewId(view);
            item["name"] = sage::render::DebugViewName(view);
            item["hint"] = sage::render::DebugViewHint(view);
            list[i + 1] = item;
        }
        return list;
    });

    // Материал, собранный СКРИПТОМ, без файла на диске. Возвращает его же —
    // поля правятся сразу (см. usertype Material), назначается он обычным
    // SetMaterial по тому же имени.
    //
    //   local m = sage.render.NewMaterial("demo/gold")
    //   m.Albedo = Vec3(0.94, 0.78, 0.36); m.Metallic = 1.0; m.Roughness = 0.18
    //   sage.render.SetMaterial(obj, "demo/gold")
    //
    // Повторный вызов с тем же именем отдаёт ТОТ ЖЕ материал, а не чистый: так
    // сетку материалов можно строить в цикле, не заводя её заранее.
    Bind("render", "NewMaterial", "NewMaterial",
         [](const std::string& name) -> std::shared_ptr<Material> {
             if (name.empty()) throw std::runtime_error("NewMaterial: имя не может быть пустым");
             return ResourceManager::Instance().MakeMaterial(name);
         });

    // Материал по пути — тот самый разделяемый экземпляр из кэша. Им правят
    // ассет проекта на ходу: изменение видно всем объектам с этим материалом.
    Bind("render", "GetMaterial", "GetMaterial",
         [](const std::string& path) -> std::shared_ptr<Material> {
             return ResourceManager::Instance().GetMaterial(path);
         });

    // Материал, которым покрашен объект (nil — материала нет, объект рисуется
    // своим цветом). Нужен, чтобы не хранить имена материалов в скрипте
    // параллельно с движком.
    Bind("render", "MaterialOf", "MaterialOf",
         [](GameObject& obj) -> std::shared_ptr<Material> { return obj.Renderer().MaterialPtr; });

    // Подтянуть текстуры материала по проставленным путям. Отдельным вызовом,
    // а не на каждое присваивание пути: у материала до шести карт, и грузить
    // его шесть раз подряд ради последней — это шесть проходов по диску.
    Bind("render", "ResolveMaterialTextures", "ResolveMaterialTextures",
         [](const std::shared_ptr<Material>& mat) {
             if (mat) ResourceManager::Instance().ResolveMaterialTextures(*mat);
         });

    // Материал сущности: путь к .sagemat. Пусто — снять материал и вернуться к
    // собственному цвету. Через него игра и назначает свой шейдер.
    Bind("render", "SetMaterial", "SetMaterial", [](GameObject& obj, const std::string& path) {
        MeshRendererComponent& mr = obj.Renderer();
        mr.MaterialPath = path;
        mr.MaterialPtr = path.empty() ? nullptr : ResourceManager::Instance().GetMaterial(path);
    });

    // Материал ОДНОЙ ЧАСТИ модели (подмеша). Нужен ровно тем, для чего слоты и
    // заведены: у персонажа сменить куртку, не трогая кожу и глаза.
    //
    // Номер части — с ЕДИНИЦЫ, как всё в Lua. Пустой путь возвращает часть к
    // материалу объекта, а не делает её невидимой: слот — это уточнение, а не
    // выключатель (см. MaterialForSubmesh).
    Bind("render", "SetSubmeshMaterial", "SetSubmeshMaterial",
         [](GameObject& obj, int part, const std::string& path) {
             MeshRendererComponent& mr = obj.Renderer();
             const size_t count = mr.MeshPtr ? mr.MeshPtr->SubmeshCount() : 0;
             if (part < 1 || (size_t)part > count) {
                 throw std::runtime_error("SetSubmeshMaterial: у модели " +
                                          std::to_string(count) + " частей, запрошена " +
                                          std::to_string(part));
             }
             if (mr.Slots.size() < count) mr.Slots.resize(count);
             MaterialSlot& slot = mr.Slots[(size_t)part - 1];
             slot.Path = path;
             slot.Ptr = path.empty() ? nullptr : ResourceManager::Instance().GetMaterial(path);
         });

    // Сколько частей у модели: без этого числа предыдущий вызов пришлось бы
    // звать наугад и ловить ошибку.
    Bind("render", "SubmeshCount", "SubmeshCount", [](GameObject& obj) -> int {
        const MeshRendererComponent& mr = obj.Renderer();
        return mr.MeshPtr ? (int)mr.MeshPtr->SubmeshCount() : 0;
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


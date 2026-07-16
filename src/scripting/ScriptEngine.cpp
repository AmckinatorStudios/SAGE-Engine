#include "ScriptEngine.h"
#include "../core/Log.h"
#include "../core/KeyNames.h"
#include "../asset/AssetManager.h"
#include "../render/ParticlePresets.h"
#include <algorithm>
#include <any>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

// Строковое имя якоря (как в C++ UIAnchor) -> значение. Нераспознанное имя —
// предупреждение и Center (не бросаем: опечатка в строке не должна ронять
// весь скрипт игры).
UIAnchor ParseUIAnchor(const std::string& name) {
    if (name == "TopLeft") return UIAnchor::TopLeft;
    if (name == "TopCenter") return UIAnchor::TopCenter;
    if (name == "TopRight") return UIAnchor::TopRight;
    if (name == "CenterLeft") return UIAnchor::CenterLeft;
    if (name == "Center") return UIAnchor::Center;
    if (name == "CenterRight") return UIAnchor::CenterRight;
    if (name == "BottomLeft") return UIAnchor::BottomLeft;
    if (name == "BottomCenter") return UIAnchor::BottomCenter;
    if (name == "BottomRight") return UIAnchor::BottomRight;
    LOG_WARN("Lua") << "UI: неизвестный Anchor '" << name << "', использую Center";
    return UIAnchor::Center;
}

// Поля, общие для всех виджетов (Anchor/Offset/Size/Visible) — читаются из
// props-таблицы, если присутствуют; отсутствующие остаются значениями по
// умолчанию виджета. Offset/Size ожидаются как Vec2 (glm::vec2), тем же
// типом, что уже используют билборды в этом файле.
void ApplyCommonUiProps(UIElement& el, const sol::table& props) {
    if (sol::optional<std::string> anchor = props["Anchor"]) el.Anchor = ParseUIAnchor(*anchor);
    if (sol::optional<glm::vec2> offset = props["Offset"]) el.Offset = *offset;
    if (sol::optional<glm::vec2> size = props["Size"]) el.Size = *size;
    if (sol::optional<bool> visible = props["Visible"]) el.Visible = *visible;
}

// --- Lua <-> JSON конвертация для SaveTable/LoadTable (см. ниже) ---
// Таблица без дырок с ключами 1..N (t.size() == реальное число элементов) —
// JSON-массив; иначе — JSON-объект (строковые/числовые ключи как строки).
// Нераспознанные типы значений (функции, userdata и т.п.) молча становятся
// null — сохранять их и не имеет смысла.
nlohmann::json LuaValueToJson(const sol::object& v);

nlohmann::json LuaTableToJson(const sol::table& t) {
    size_t n = t.size();
    size_t total = 0;
    for (const auto& kv : t) { (void)kv; ++total; }
    bool isArray = (n > 0) && (n == total);

    if (isArray) {
        nlohmann::json arr = nlohmann::json::array();
        for (size_t i = 1; i <= n; ++i) arr.push_back(LuaValueToJson(t[i]));
        return arr;
    }
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& kv : t) {
        std::string key;
        if (kv.first.is<std::string>()) key = kv.first.as<std::string>();
        else if (kv.first.is<double>()) key = std::to_string(kv.first.as<double>());
        else continue;
        obj[key] = LuaValueToJson(kv.second);
    }
    return obj;
}

nlohmann::json LuaValueToJson(const sol::object& v) {
    switch (v.get_type()) {
        case sol::type::number:  return v.as<double>();
        case sol::type::string:  return v.as<std::string>();
        case sol::type::boolean: return v.as<bool>();
        case sol::type::table:   return LuaTableToJson(v.as<sol::table>());
        default:                 return nullptr;
    }
}

sol::object JsonToLuaValue(sol::state& lua, const nlohmann::json& j) {
    if (j.is_boolean()) return sol::make_object(lua, j.get<bool>());
    if (j.is_number())  return sol::make_object(lua, j.get<double>());
    if (j.is_string())  return sol::make_object(lua, j.get<std::string>());
    if (j.is_array()) {
        sol::table t = lua.create_table();
        int i = 1;
        for (const auto& el : j) t[i++] = JsonToLuaValue(lua, el);
        return t;
    }
    if (j.is_object()) {
        sol::table t = lua.create_table();
        for (auto it = j.begin(); it != j.end(); ++it) t[it.key()] = JsonToLuaValue(lua, it.value());
        return t;
    }
    return sol::lua_nil;
}

} // namespace

ScriptEngine::ScriptEngine() {
    m_lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                          sol::lib::table, sol::lib::coroutine);
    RegisterEngineApi();
}

void ScriptEngine::RegisterEngineApi() {
    // glm::vec3 — доступен из Lua как обычная таблица с полями x/y/z, плюс
    // арифметика (+, -, унарный минус, умножение/деление на число) и пара
    // геометрических хелперов — без этого любая игровая математика (движение,
    // направления, дистанции) была бы мучением через отдельные x/y/z-поля.
    m_lua.new_usertype<glm::vec3>("Vec3",
        sol::constructors<glm::vec3(), glm::vec3(float, float, float)>(),
        "x", &glm::vec3::x,
        "y", &glm::vec3::y,
        "z", &glm::vec3::z,
        sol::meta_function::addition, [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
        sol::meta_function::subtraction, [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
        sol::meta_function::unary_minus, [](const glm::vec3& a) { return -a; },
        sol::meta_function::multiplication, [](const glm::vec3& a, float s) { return a * s; },
        sol::meta_function::division, [](const glm::vec3& a, float s) { return a / s; },
        sol::meta_function::to_string, [](const glm::vec3& a) {
            return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ", " + std::to_string(a.z) + ")";
        },
        "Length", [](const glm::vec3& a) { return glm::length(a); },
        "Normalized", [](const glm::vec3& a) {
            float len = glm::length(a);
            return len > 0.0001f ? a / len : a;
        },
        "Distance", [](const glm::vec3& a, const glm::vec3& b) { return glm::length(b - a); },
        "Dot", [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); }
    );

    // Vec2 — размеры билбордов (Size) и прочая 2D-математика (экранные
    // координаты, UV). Минимальный набор — арифметика та же, что у Vec3.
    m_lua.new_usertype<glm::vec2>("Vec2",
        sol::constructors<glm::vec2(), glm::vec2(float, float)>(),
        "x", &glm::vec2::x,
        "y", &glm::vec2::y,
        sol::meta_function::addition, [](const glm::vec2& a, const glm::vec2& b) { return a + b; },
        sol::meta_function::subtraction, [](const glm::vec2& a, const glm::vec2& b) { return a - b; },
        sol::meta_function::multiplication, [](const glm::vec2& a, float s) { return a * s; },
        sol::meta_function::to_string, [](const glm::vec2& a) {
            return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ")";
        }
    );

    // Vec4 — цвета с альфа-каналом (частицы, тонирование билбордов). Здесь
    // намеренно нет геометрических операций (Length/Normalized/Dot) — Vec4
    // в этом движке используется только как rgba, не как направление/точка.
    m_lua.new_usertype<glm::vec4>("Vec4",
        sol::constructors<glm::vec4(), glm::vec4(float, float, float, float)>(),
        "x", &glm::vec4::x,
        "y", &glm::vec4::y,
        "z", &glm::vec4::z,
        "w", &glm::vec4::w,
        sol::meta_function::to_string, [](const glm::vec4& a) {
            return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ", "
                 + std::to_string(a.z) + ", " + std::to_string(a.w) + ")";
        }
    );

    // Transform — позиция/поворот/масштаб объекта, доступны на чтение и запись
    m_lua.new_usertype<::Transform>("Transform",
        "Position", &::Transform::Position,
        "Rotation", &::Transform::Rotation,
        "Scale", &::Transform::Scale
    );

    // GameObject — то, что скрипт получает как "entity" в OnUpdate/OnStart,
    // или что возвращают SpawnObject/FindObject
    m_lua.new_usertype<GameObject>("GameObject",
        "Id", sol::readonly(&GameObject::Id),
        "Name", &GameObject::Name,
        "Transform", &GameObject::TransformComponent,
        "Color", &GameObject::Color,
        // Свободный тег объекта — фильтрация в рендер-проходах по строке
        // (например "terrain"/"water"), смысл которой знает только игра.
        "Tag", &GameObject::Tag,
        // entity.Lua — свободная Lua-таблица для произвольных данных игры на
        // этом объекте, без правки заголовков движка (см. Scene.h::LuaData).
        // Геттер лениво создаёт таблицу при первом обращении; сеттер
        // позволяет целиком заменить её (entity.Lua = {...}).
        "Lua", sol::property(
            [this](GameObject& obj) -> sol::table {
                if (!obj.LuaData.has_value()) {
                    obj.LuaData = m_lua.create_table();
                }
                return std::any_cast<sol::table>(obj.LuaData);
            },
            [](GameObject& obj, sol::table value) {
                obj.LuaData = std::move(value);
            })
    );

    // Простая функция логирования, чтобы скрипты могли печатать отладочную информацию
    m_lua.set_function("log", [](const std::string& message) {
        LOG_INFO("Lua") << message;
    });

    // --- Сцена: спавн/поиск/удаление объектов из Lua (доступно после BindScene) ---
    m_lua.set_function("SpawnObject", [this](const std::string& name) -> GameObject& {
        if (!m_scene) throw std::runtime_error("SpawnObject: сцена не привязана (ScriptEngine::BindScene не вызван)");
        GameObject& obj = m_scene->CreateObject(name);
        LOG_INFO("Lua") << "Создан объект: " << name << " (id " << obj.Id << ")";
        return obj;
    });

    m_lua.set_function("FindObject", [this](const std::string& name) -> GameObject* {
        return m_scene ? m_scene->FindByName(name) : nullptr;
    });

    m_lua.set_function("DestroyObject", [this](int id) {
        if (!m_scene) throw std::runtime_error("DestroyObject: сцена не привязана (ScriptEngine::BindScene не вызван)");
        // Помечаем зависимые ScriptInstance мёртвыми ДО RemoveObject — ниже
        // она реально освобождает GameObject (Scene хранит его в unique_ptr),
        // поэтому сравнение instance.Object->Id должно случиться, пока объект
        // ещё жив, иначе само это сравнение уже было бы use-after-free.
        // UpdateAll() пропустит помеченные записи и уберёт их после текущего
        // прохода, не разыменовывая освобождённую память. ВАЖНО: если скрипт
        // вызывает DestroyObject(entity.Id) сам про себя, это должно быть
        // последним действием в OnUpdate — entity после этого момента (даже в
        // том же вызове) ссылается на уже уничтоженный объект.
        for (auto& instance : m_instances) {
            if (instance.Object && instance.Object->Id == id) instance.Dead = true;
        }
        m_scene->RemoveObject(id);
    });

    // Удобный хелпер: даёт заспавненному объекту видимый куб-меш, чтобы его
    // сразу было видно на сцене без ручной возни с MeshRef/AssetManager
    m_lua.set_function("SetMeshCube", [](GameObject& obj) {
        obj.MeshRefComponent = MeshRef{MeshRef::Type::Cube, ""};
        obj.MeshComponent = AssetManager::Instance().Cube().Shared();
    });

    // То же самое, но грузит .obj модель (кэшируется AssetManager'ом —
    // одна и та же модель, запрошенная из нескольких скриптов, не
    // перечитывается с диска). Бросает ошибку, если файл не найден/битый.
    m_lua.set_function("SetMeshModel", [](GameObject& obj, const std::string& path) {
        obj.MeshRefComponent = MeshRef{MeshRef::Type::Model, path};
        obj.MeshComponent = AssetManager::Instance().LoadModel(path).Shared();
    });

    // Асинхронный вариант: модель грузится в фоне (JobSystem), а меш
    // назначается объекту, как только он готов (GL-загрузка в главном потоке).
    // Кадр при этом не фризит — объект просто «появляется» через несколько
    // кадров. Готовность опрашивается в UpdateAll(); если объект удалят раньше,
    // результат тихо отбрасывается (ищем по Id).
    m_lua.set_function("SetMeshModelAsync", [this](GameObject& obj, const std::string& path) {
        obj.MeshRefComponent = MeshRef{MeshRef::Type::Model, path};
        m_pendingMeshes.push_back({obj.Id, AssetManager::Instance().LoadModelAsync(path), path});
    });

    // Процедурный/динамический меш — ключевой энейблер для игр без готовой
    // геометрии на диске (например, воксельный мир, полностью реализованный
    // на Lua: игра сама хранит блоки и считает меш чанка, а движок только
    // грузит готовые вершины на GPU). Переиспользует существующие MeshData/
    // Mesh(MeshData)/Mesh::Reload — никакой новой C++ геометрии, только
    // маршалинг Lua-таблиц.
    //
    // vertices — массив вершин, каждая вершина — массив из 8 чисел
    // {x,y,z, nx,ny,nz, u,v} (позиция, нормаль, UV). indices — плоский массив
    // индексов треугольников, 1-based (как везде в Lua) — сами конвертируем
    // в 0-based. Если у объекта уже есть меш — перезагружаем его НА МЕСТЕ
    // (Mesh::Reload), не создавая новый GL-объект каждый вызов: так можно
    // звать SetMeshData каждый раз, когда чанк поменялся, без накопления
    // мусора. Иначе создаём новый Mesh.
    //
    // Процедурный меш не привязан к MeshRefComponent (Type::None) — в
    // отличие от куба/модели, у него нет исходного файла, который
    // AssetManager мог бы перечитать при загрузке сцены; игра, которая
    // хранит свою геометрию сама (как воксельный мир на Lua), отвечает и за
    // её восстановление после загрузки сама.
    m_lua.set_function("SetMeshData", [](GameObject& obj, sol::table vertices, sol::table indices) {
        MeshData data;
        data.Vertices.reserve(vertices.size());
        for (size_t i = 1; i <= vertices.size(); ++i) {
            sol::table v = vertices[i];
            Vertex vert{};
            vert.Position  = {v.get_or(1, 0.0f), v.get_or(2, 0.0f), v.get_or(3, 0.0f)};
            vert.Normal    = {v.get_or(4, 0.0f), v.get_or(5, 0.0f), v.get_or(6, 0.0f)};
            vert.TexCoords = {v.get_or(7, 0.0f), v.get_or(8, 0.0f)};
            data.Vertices.push_back(vert);
        }
        data.Indices.reserve(indices.size());
        for (size_t i = 1; i <= indices.size(); ++i) {
            int idx = indices.get<int>(i);
            data.Indices.push_back(static_cast<unsigned int>(idx - 1)); // Lua 1-based -> C++ 0-based
        }

        obj.MeshRefComponent = MeshRef{MeshRef::Type::None, ""};
        if (obj.MeshComponent) {
            obj.MeshComponent->Reload(data);
        } else {
            obj.MeshComponent = std::make_shared<Mesh>(data);
        }
    });

    // --- Освещение: доступно после BindScene (LightingEnvironment — часть
    // Scene, см. scene/Light.h). Даёт скриптам полный контроль над картинкой
    // без правки движка/шейдеров — тот же UploadLighting каждый кадр читает
    // те же поля, что здесь пишет Lua (день/ночь, фонари и т.п. — целиком на
    // стороне игры). ---
    m_lua.set_function("SetAmbient", [this](glm::vec3 sky, glm::vec3 ground, float strength) {
        if (!m_scene) throw std::runtime_error("SetAmbient: сцена не привязана (ScriptEngine::BindScene не вызван)");
        m_scene->Lighting.SkyColor = sky;
        m_scene->Lighting.GroundColor = ground;
        m_scene->Lighting.AmbientStrength = strength;
    });
    m_lua.set_function("SetSun", [this](glm::vec3 direction, glm::vec3 color, float intensity) {
        if (!m_scene) throw std::runtime_error("SetSun: сцена не привязана (ScriptEngine::BindScene не вызван)");
        m_scene->Lighting.Sun.Direction = direction;
        m_scene->Lighting.Sun.Color = color;
        m_scene->Lighting.Sun.Intensity = intensity;
    });
    // Возвращает индекс добавленного света (0-based) — использовать как id
    // для SetPointLight. Индексы стабильны, пока не вызван ClearPointLights()
    // (точечного удаления одного света нет — см. комментарий в .h).
    m_lua.set_function("AddPointLight", [this](glm::vec3 pos, glm::vec3 color, float intensity, float range) -> int {
        if (!m_scene) throw std::runtime_error("AddPointLight: сцена не привязана (ScriptEngine::BindScene не вызван)");
        auto& lights = m_scene->Lighting.PointLights;
        if ((int)lights.size() >= LightingEnvironment::MaxPointLights) {
            LOG_WARN("Lua") << "AddPointLight: достигнут лимит MaxPointLights ("
                            << LightingEnvironment::MaxPointLights << "), свет проигнорирован";
            return -1;
        }
        PointLight light;
        light.Position = pos; light.Color = color; light.Intensity = intensity; light.Range = range;
        lights.push_back(light);
        return (int)lights.size() - 1;
    });
    m_lua.set_function("SetPointLight", [this](int id, glm::vec3 pos, glm::vec3 color, float intensity, float range) {
        if (!m_scene) throw std::runtime_error("SetPointLight: сцена не привязана (ScriptEngine::BindScene не вызван)");
        auto& lights = m_scene->Lighting.PointLights;
        if (id < 0 || id >= (int)lights.size()) return; // тихо игнорируем невалидный id
        lights[id].Position = pos; lights[id].Color = color;
        lights[id].Intensity = intensity; lights[id].Range = range;
    });
    m_lua.set_function("ClearPointLights", [this]() {
        if (!m_scene) throw std::runtime_error("ClearPointLights: сцена не привязана (ScriptEngine::BindScene не вызван)");
        m_scene->Lighting.PointLights.clear();
    });

    // --- Ввод: именованные действия движка (см. core/InputMap.h), доступно
    // после BindInput. Скрипты читают тот же ввод, что и C++-код игры —
    // никакого параллельного дублирования раскладки клавиш. ---
    m_lua.set_function("IsActionDown", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name) && m_input->IsDown(name);
    });
    m_lua.set_function("WasActionPressed", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name) && m_input->WasPressed(name);
    });
    m_lua.set_function("WasActionReleased", [this](const std::string& name) -> bool {
        return m_input && m_input->Has(name) && m_input->WasReleased(name);
    });

    // Регистрация именованных действий и их привязок из Lua — раньше это
    // жило только как хардкод в C++ (game/GameActions.h); теперь игра может
    // целиком объявить свою раскладку скриптом. RegisterAction создаёт
    // действие (без привязок — их даёт BindAction), BindAction добавляет к
    // нему физический источник по имени (буквы/цифры/F1-F12/SPACE/TAB/
    // ENTER/ESCAPE/стрелки/MOUSE_LEFT/MOUSE_RIGHT/MOUSE_MIDDLE/WHEEL_UP/
    // WHEEL_DOWN — полный список см. core/KeyNames.h::Parse), одно действие
    // может иметь несколько привязок (несколько вызовов BindAction). Логи
    // и переопределение раскладки без пересборки — из того же keybindings.cfg,
    // что и раньше, теперь через LoadKeybindingsFromFile.
    m_lua.set_function("RegisterAction", [this](const std::string& name) {
        if (!m_input) throw std::runtime_error("RegisterAction: ввод не привязан (ScriptEngine::BindInput не вызван)");
        m_input->Register(name);
    });
    m_lua.set_function("BindAction", [this](const std::string& name, const std::string& bindingName) {
        if (!m_input) throw std::runtime_error("BindAction: ввод не привязан (ScriptEngine::BindInput не вызван)");
        if (!m_input->Has(name)) throw std::runtime_error("BindAction: действие '" + name + "' не зарегистрировано — забыт RegisterAction?");
        auto binding = KeyNames::Parse(bindingName);
        if (!binding) {
            LOG_WARN("Lua") << "BindAction: нераспознанная привязка '" << bindingName << "' для '" << name << "'";
            return;
        }
        m_input->Get(name).Bind(*binding);
    });
    m_lua.set_function("LoadKeybindingsFromFile", [this](const std::string& path) {
        if (!m_input) throw std::runtime_error("LoadKeybindingsFromFile: ввод не привязан (ScriptEngine::BindInput не вызван)");
        KeyNames::LoadBindingsFromFile(*m_input, path);
    });

    // --- Камера: доступно после BindCamera. Position — обычное поле, но
    // Yaw/Pitch выставлены через property-функции, которые сразу пересчитывают
    // Front/Right/Up вызовом ProcessMouse(0,0) — тем же приёмом, что уже
    // использует ApplyDebugEnvOverrides() в main.cpp после ручной правки угла.
    // Без этого камера "смотрела" бы в старом направлении до следующего
    // движения мыши игроком. ---
    m_lua.new_usertype<Camera>("Camera",
        "Position", &Camera::Position,
        "Front", sol::readonly(&Camera::Front),
        "Right", sol::readonly(&Camera::Right),
        "Up", sol::readonly(&Camera::Up),
        "Fov", &Camera::Fov,
        "MovementSpeed", &Camera::MovementSpeed,
        "NearClip", &Camera::NearClip,
        "FarClip", &Camera::FarClip,
        "Yaw", sol::property(
            [](const Camera& c) { return c.Yaw; },
            [](Camera& c, float yaw) { c.Yaw = yaw; c.ProcessMouse(0.0f, 0.0f); }),
        "Pitch", sol::property(
            [](const Camera& c) { return c.Pitch; },
            [](Camera& c, float pitch) { c.Pitch = pitch; c.ProcessMouse(0.0f, 0.0f); })
    );

    m_lua.set_function("GetCamera", [this]() -> Camera& {
        if (!m_camera) throw std::runtime_error("GetCamera: камера не привязана (ScriptEngine::BindCamera не вызван)");
        return *m_camera;
    });

    // --- Частицы: доступно после BindParticles. ParticleConfig — те же поля,
    // что и ParticleEmitterConfig в C++ (см. render/Particle.h), плюс готовые
    // пресеты ParticlePresets.* (те же, что использует The Boat в main.cpp) —
    // Lua-скрипт может взять пресет как основу и подправить пару полей,
    // вместо того чтобы описывать весь конфиг с нуля. ---
    m_lua.new_usertype<ParticleEmitterConfig>("ParticleConfig",
        sol::constructors<ParticleEmitterConfig()>(),
        "DirectionMin", &ParticleEmitterConfig::DirectionMin,
        "DirectionMax", &ParticleEmitterConfig::DirectionMax,
        "SpeedMin", &ParticleEmitterConfig::SpeedMin,
        "SpeedMax", &ParticleEmitterConfig::SpeedMax,
        "Gravity", &ParticleEmitterConfig::Gravity,
        "LifetimeMin", &ParticleEmitterConfig::LifetimeMin,
        "LifetimeMax", &ParticleEmitterConfig::LifetimeMax,
        "StartSizeMin", &ParticleEmitterConfig::StartSizeMin,
        "StartSizeMax", &ParticleEmitterConfig::StartSizeMax,
        "EndSizeMin", &ParticleEmitterConfig::EndSizeMin,
        "EndSizeMax", &ParticleEmitterConfig::EndSizeMax,
        "StartColor", &ParticleEmitterConfig::StartColor,
        "EndColor", &ParticleEmitterConfig::EndColor,
        "AngularVelocityMax", &ParticleEmitterConfig::AngularVelocityMax,
        "EmissionRate", &ParticleEmitterConfig::EmissionRate
    );

    sol::table presets = m_lua.create_table();
    presets.set_function("WaterSplash", &ParticlePresets::WaterSplash);
    presets.set_function("Smoke", &ParticlePresets::Smoke);
    presets.set_function("BlockBreak", &ParticlePresets::BlockBreak);
    presets.set_function("StoveEmbers", &ParticlePresets::StoveEmbers);
    m_lua["ParticlePresets"] = presets;

    // Разовый залп частиц в мировой точке — см. ParticleSystem::Burst
    m_lua.set_function("EmitParticles", [this](const ParticleEmitterConfig& config, glm::vec3 pos, int count) {
        if (!m_particles) throw std::runtime_error("EmitParticles: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->Burst(config, pos, count);
    });
    // Непрерывная струя (дым, искры) — создаётся выключенной, включай
    // SetParticleStreamActive(id, true) отдельно (см. ParticleSystem::CreateStream)
    m_lua.set_function("CreateParticleStream", [this](const std::string& id, const ParticleEmitterConfig& config, glm::vec3 pos) {
        if (!m_particles) throw std::runtime_error("CreateParticleStream: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->CreateStream(id, config, pos);
    });
    m_lua.set_function("SetParticleStreamActive", [this](const std::string& id, bool active) {
        if (!m_particles) throw std::runtime_error("SetParticleStreamActive: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->SetStreamActive(id, active);
    });
    m_lua.set_function("SetParticleStreamPosition", [this](const std::string& id, glm::vec3 pos) {
        if (!m_particles) throw std::runtime_error("SetParticleStreamPosition: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->SetStreamPosition(id, pos);
    });
    m_lua.set_function("RemoveParticleStream", [this](const std::string& id) {
        if (!m_particles) throw std::runtime_error("RemoveParticleStream: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->RemoveStream(id);
    });

    // --- Билборды: доступно после BindBillboards. Спрайт без texturePath
    // рисуется сплошным цветом Tint (см. BillboardSprite) — удобно для
    // маркеров/индикаторов, для которых не хочется готовить текстуру. ---
    m_lua.set_function("AddBillboard", [this](glm::vec3 pos, glm::vec2 size, sol::optional<std::string> texturePath) -> int {
        if (!m_billboards) throw std::runtime_error("AddBillboard: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        BillboardSprite sprite;
        sprite.WorldPos = pos;
        sprite.Size = size;
        if (texturePath) sprite.SpriteTexture = GetOrLoadBillboardTexture(*texturePath);
        return m_billboards->Add(sprite);
    });
    m_lua.set_function("RemoveBillboard", [this](int id) {
        if (!m_billboards) throw std::runtime_error("RemoveBillboard: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->Remove(id);
    });
    m_lua.set_function("SetBillboardPosition", [this](int id, glm::vec3 pos) {
        if (!m_billboards) throw std::runtime_error("SetBillboardPosition: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->SetPosition(id, pos);
    });
    m_lua.set_function("SetBillboardVisible", [this](int id, bool visible) {
        if (!m_billboards) throw std::runtime_error("SetBillboardVisible: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->SetVisible(id, visible);
    });
    m_lua.set_function("SetBillboardTint", [this](int id, glm::vec4 tint) {
        if (!m_billboards) throw std::runtime_error("SetBillboardTint: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->SetTint(id, tint);
    });

    // --- Звук: доступно после BindAudio. Скрипты (катсцены, события уровня)
    // могут проигрывать эффекты/музыку и рулить громкостью. Если аудио-
    // устройство недоступно (headless), вызовы безопасно ничего не делают. ---
    m_lua.set_function("PlaySound", [this](const std::string& path, sol::optional<float> volume) {
        if (!m_audio) throw std::runtime_error("PlaySound: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->PlaySound2D(path, volume.value_or(1.0f));
    });
    m_lua.set_function("PlaySound3D", [this](const std::string& path, glm::vec3 pos, sol::optional<float> volume) {
        if (!m_audio) throw std::runtime_error("PlaySound3D: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->PlaySound3D(path, pos, volume.value_or(1.0f));
    });
    m_lua.set_function("PlayMusic", [this](const std::string& path, sol::optional<float> volume) {
        if (!m_audio) throw std::runtime_error("PlayMusic: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->PlayMusic(path, volume.value_or(1.0f), true);
    });
    m_lua.set_function("StopMusic", [this]() {
        if (!m_audio) throw std::runtime_error("StopMusic: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->StopMusic();
    });
    m_lua.set_function("SetMasterVolume", [this](float volume) {
        if (!m_audio) throw std::runtime_error("SetMasterVolume: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->SetMasterVolume(volume);
    });

    // --- UI: доступно после BindUI. Раньше UICanvas собирался только руками
    // в C++ (main.cpp) — теперь HUD/меню/панели настроек можно целиком
    // построить и обновлять из Lua: CreateCanvas заводит именованный канвас,
    // Add* добавляет в него виджет и возвращает целочисленный id, SetWidget*
    // обновляет уже созданный виджет по id (типично — каждый кадр из
    // OnUpdate, как и обновление игровых данных). Общие поля (Anchor/Offset/
    // Size/Visible) — см. ApplyCommonUiProps выше; удобно передавать как
    // props = {Anchor="TopLeft", Offset=Vec2.new(16,16), Size=Vec2.new(180,16)}. ---
    m_lua.set_function("CreateCanvas", [this](const std::string& name) {
        if (!m_ui) throw std::runtime_error("CreateCanvas: UI не привязан (ScriptEngine::BindUI не вызван)");
        m_ui->CreateCanvas(name);
    });
    m_lua.set_function("SetCanvasReferenceSize", [this](const std::string& name, float w, float h) {
        if (!m_ui) throw std::runtime_error("SetCanvasReferenceSize: UI не привязан (ScriptEngine::BindUI не вызван)");
        m_ui->CreateCanvas(name).ReferenceSize = {w, h};
    });
    m_lua.set_function("SetCanvasVisible", [this](const std::string& name, bool visible) {
        if (!m_ui) throw std::runtime_error("SetCanvasVisible: UI не привязан (ScriptEngine::BindUI не вызван)");
        m_ui->CreateCanvas(name).Visible = visible;
    });

    m_lua.set_function("AddPanel", [this](const std::string& canvas, sol::table props) -> int {
        if (!m_ui) throw std::runtime_error("AddPanel: UI не привязан (ScriptEngine::BindUI не вызван)");
        auto* w = m_ui->CreateCanvas(canvas).Add<UIPanel>();
        ApplyCommonUiProps(*w, props);
        if (sol::optional<glm::vec3> c = props["Color"]) w->Color = *c;
        if (sol::optional<float> a = props["Alpha"]) w->Alpha = *a;
        if (sol::optional<float> ot = props["OutlineThickness"]) w->OutlineThickness = *ot;
        if (sol::optional<glm::vec3> oc = props["OutlineColor"]) w->OutlineColor = *oc;
        if (sol::optional<float> oa = props["OutlineAlpha"]) w->OutlineAlpha = *oa;
        return m_ui->RegisterWidget(w);
    });
    m_lua.set_function("AddLabel", [this](const std::string& canvas, sol::table props) -> int {
        if (!m_ui) throw std::runtime_error("AddLabel: UI не привязан (ScriptEngine::BindUI не вызван)");
        auto* w = m_ui->CreateCanvas(canvas).Add<UILabel>();
        ApplyCommonUiProps(*w, props);
        if (sol::optional<std::string> t = props["Text"]) w->Text = *t;
        if (sol::optional<float> s = props["Scale"]) w->Scale = *s;
        if (sol::optional<glm::vec3> c = props["Color"]) w->Color = *c;
        if (sol::optional<bool> ci = props["CenterInSize"]) w->CenterInSize = *ci;
        return m_ui->RegisterWidget(w);
    });
    m_lua.set_function("AddProgressBar", [this](const std::string& canvas, sol::table props) -> int {
        if (!m_ui) throw std::runtime_error("AddProgressBar: UI не привязан (ScriptEngine::BindUI не вызван)");
        auto* w = m_ui->CreateCanvas(canvas).Add<UIProgressBar>();
        ApplyCommonUiProps(*w, props);
        if (sol::optional<std::string> l = props["Label"]) w->Label = *l;
        if (sol::optional<glm::vec3> fc = props["FillColor"]) w->FillColor = *fc;
        if (sol::optional<glm::vec3> bc = props["BackColor"]) w->BackColor = *bc;
        if (sol::optional<float> ba = props["BackAlpha"]) w->BackAlpha = *ba;
        if (sol::optional<float> ls = props["LabelScale"]) w->LabelScale = *ls;
        return m_ui->RegisterWidget(w);
    });
    m_lua.set_function("AddSprite", [this](const std::string& canvas, sol::table props) -> int {
        if (!m_ui) throw std::runtime_error("AddSprite: UI не привязан (ScriptEngine::BindUI не вызван)");
        auto* w = m_ui->CreateCanvas(canvas).Add<UISprite>();
        ApplyCommonUiProps(*w, props);
        if (sol::optional<glm::vec4> tint = props["Tint"]) w->Tint = *tint;
        if (sol::optional<bool> ka = props["KeepAspect"]) w->KeepAspect = *ka;
        int id = m_ui->RegisterWidget(w);
        if (sol::optional<std::string> path = props["Texture"]) {
            Asset<Texture> tex = AssetManager::Instance().LoadTexture(*path, TextureFilter::Bilinear);
            w->SpriteTexture = tex.Get();
            m_ui->KeepTextureAlive(id, tex);
        }
        return id;
    });
    m_lua.set_function("AddButton", [this](const std::string& canvas, sol::table props) -> int {
        if (!m_ui) throw std::runtime_error("AddButton: UI не привязан (ScriptEngine::BindUI не вызван)");
        auto* w = m_ui->CreateCanvas(canvas).Add<UIButton>();
        ApplyCommonUiProps(*w, props);
        if (sol::optional<std::string> l = props["Label"]) w->Label = *l;
        if (sol::optional<glm::vec3> nc = props["NormalColor"]) w->NormalColor = *nc;
        if (sol::optional<glm::vec3> hc = props["HoverColor"]) w->HoverColor = *hc;
        if (sol::optional<glm::vec3> pc = props["PressedColor"]) w->PressedColor = *pc;
        if (sol::optional<float> a = props["Alpha"]) w->Alpha = *a;
        if (sol::optional<glm::vec3> tc = props["TextColor"]) w->TextColor = *tc;
        if (sol::optional<float> ts = props["TextScale"]) w->TextScale = *ts;
        if (sol::optional<float> ot = props["OutlineThickness"]) w->OutlineThickness = *ot;
        if (sol::optional<glm::vec3> oc = props["OutlineColor"]) w->OutlineColor = *oc;
        if (sol::optional<sol::protected_function> onClick = props["OnClick"]) {
            sol::protected_function fn = *onClick;
            w->OnClick = [fn]() {
                auto result = fn();
                if (!result.valid()) {
                    sol::error err = result;
                    LOG_ERROR("ScriptEngine") << "Ошибка в UI OnClick: " << err.what();
                }
            };
        }
        return m_ui->RegisterWidget(w);
    });
    m_lua.set_function("AddToggle", [this](const std::string& canvas, sol::table props) -> int {
        if (!m_ui) throw std::runtime_error("AddToggle: UI не привязан (ScriptEngine::BindUI не вызван)");
        auto* w = m_ui->CreateCanvas(canvas).Add<UIToggle>();
        ApplyCommonUiProps(*w, props);
        if (sol::optional<std::string> l = props["Label"]) w->Label = *l;
        if (sol::optional<bool> v = props["Value"]) w->Value = *v;
        if (sol::optional<glm::vec3> onc = props["OnColor"]) w->OnColor = *onc;
        if (sol::optional<glm::vec3> offc = props["OffColor"]) w->OffColor = *offc;
        if (sol::optional<glm::vec3> kc = props["KnobColor"]) w->KnobColor = *kc;
        if (sol::optional<glm::vec3> lc = props["LabelColor"]) w->LabelColor = *lc;
        if (sol::optional<float> ls = props["LabelScale"]) w->LabelScale = *ls;
        if (sol::optional<sol::protected_function> onChanged = props["OnChanged"]) {
            sol::protected_function fn = *onChanged;
            w->OnChanged = [fn](bool value) {
                auto result = fn(value);
                if (!result.valid()) {
                    sol::error err = result;
                    LOG_ERROR("ScriptEngine") << "Ошибка в UI OnChanged: " << err.what();
                }
            };
        }
        return m_ui->RegisterWidget(w);
    });

    // Обновление уже созданных виджетов по id — типично раз в кадр из
    // OnUpdate (полоски статов, текст HUD и т.п.), тем самым паттерном, что
    // и весь остальной игровой цикл на Lua. Невалидный id тихо игнорируется
    // (виджет мог быть создан на другом канвасе/ещё не существовать).
    m_lua.set_function("SetWidgetText", [this](int id, const std::string& text) {
        if (!m_ui) throw std::runtime_error("SetWidgetText: UI не привязан (ScriptEngine::BindUI не вызван)");
        UIElement* w = m_ui->GetWidget(id);
        if (!w) return;
        if (auto* label = dynamic_cast<UILabel*>(w)) label->Text = text;
        else if (auto* btn = dynamic_cast<UIButton*>(w)) btn->Label = text;
        else if (auto* bar = dynamic_cast<UIProgressBar*>(w)) bar->Label = text;
        else if (auto* toggle = dynamic_cast<UIToggle*>(w)) toggle->Label = text;
    });
    // value — число (0..1) для ProgressBar, булево для Toggle; тип должен
    // соответствовать типу виджета по id.
    m_lua.set_function("SetWidgetValue", [this](int id, sol::object value) {
        if (!m_ui) throw std::runtime_error("SetWidgetValue: UI не привязан (ScriptEngine::BindUI не вызван)");
        UIElement* w = m_ui->GetWidget(id);
        if (!w) return;
        if (auto* bar = dynamic_cast<UIProgressBar*>(w)) {
            float v = value.as<float>();
            bar->ValueSource = [v] { return v; };
        } else if (auto* toggle = dynamic_cast<UIToggle*>(w)) {
            toggle->Value = value.as<bool>();
        }
    });
    m_lua.set_function("SetWidgetVisible", [this](int id, bool visible) {
        if (!m_ui) throw std::runtime_error("SetWidgetVisible: UI не привязан (ScriptEngine::BindUI не вызван)");
        if (UIElement* w = m_ui->GetWidget(id)) w->Visible = visible;
    });
    m_lua.set_function("SetWidgetTexture", [this](int id, const std::string& path) {
        if (!m_ui) throw std::runtime_error("SetWidgetTexture: UI не привязан (ScriptEngine::BindUI не вызван)");
        auto* sprite = dynamic_cast<UISprite*>(m_ui->GetWidget(id));
        if (!sprite) return;
        Asset<Texture> tex = AssetManager::Instance().LoadTexture(path, TextureFilter::Bilinear);
        sprite->SpriteTexture = tex.Get();
        m_ui->KeepTextureAlive(id, tex);
    });

    // --- Сохранение/загрузка: произвольная Lua-таблица <-> JSON-файл на
    // диске, без необходимости изобретать свою сериализацию на каждую игру
    // (инвентарь, статы, счётчик дня и т.п.). Использует nlohmann::json —
    // ту же библиотеку, что и SceneSerializer для .sage-сцен. Таблица без
    // дырок с ключами 1..N сохраняется как JSON-массив, любая другая — как
    // JSON-объект (см. LuaTableToJson выше). SaveTable возвращает true/false
    // (успех записи); LoadTable возвращает nil, если файла нет или он битый
    // — вызывающий сам решает, что подставить по умолчанию в этом случае. ---
    m_lua.set_function("SaveTable", [](const std::string& path, sol::table data) -> bool {
        try {
            nlohmann::json j = LuaTableToJson(data);
            std::ofstream file(path);
            if (!file.is_open()) {
                LOG_ERROR("Lua") << "SaveTable: не удалось открыть файл для записи: " << path;
                return false;
            }
            file << j.dump(2);
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("Lua") << "SaveTable: ошибка сериализации (" << path << "): " << e.what();
            return false;
        }
    });
    m_lua.set_function("LoadTable", [this](const std::string& path) -> sol::object {
        std::ifstream file(path);
        if (!file.is_open()) return sol::lua_nil;
        try {
            nlohmann::json j;
            file >> j;
            return JsonToLuaValue(m_lua, j);
        } catch (const std::exception& e) {
            LOG_ERROR("Lua") << "LoadTable: ошибка разбора (" << path << "): " << e.what();
            return sol::lua_nil;
        }
    });

    // --- JSON как строка (без файла): полезно для сетевых сообщений,
    // конфигов, обмена с C++-стороной и вообще любого текстового формата.
    // Те же правила маршалинга, что и у SaveTable/LoadTable (таблица с
    // ключами 1..N -> JSON-массив, иначе -> JSON-объект; числа/строки/булевы
    // -> примитивы). JsonEncode(value, pretty?) кодирует любое Lua-значение
    // (не только таблицу) в строку; pretty=true даёт отступ в 2 пробела,
    // иначе компактный вывод. JsonDecode(str) разбирает строку обратно в
    // Lua-значение, возвращая nil при синтаксической ошибке. ---
    m_lua.set_function("JsonEncode", [](sol::object value, sol::optional<bool> pretty) -> sol::object {
        try {
            nlohmann::json j = LuaValueToJson(value);
            return sol::make_object(sol::state_view(value.lua_state()),
                                    j.dump(pretty.value_or(false) ? 2 : -1));
        } catch (const std::exception& e) {
            LOG_ERROR("Lua") << "JsonEncode: ошибка сериализации: " << e.what();
            return sol::lua_nil;
        }
    });
    m_lua.set_function("JsonDecode", [this](const std::string& text) -> sol::object {
        try {
            nlohmann::json j = nlohmann::json::parse(text);
            return JsonToLuaValue(m_lua, j);
        } catch (const std::exception& e) {
            LOG_ERROR("Lua") << "JsonDecode: ошибка разбора: " << e.what();
            return sol::lua_nil;
        }
    });

    // --- Таймеры: отложенные/повторяющиеся вызовы без ручного хранения
    // "сколько осталось" в самом скрипте. Возвращают id для CancelTimer. ---
    m_lua.set_function("Schedule", [this](float seconds, sol::protected_function fn) -> int {
        int id = m_nextTimerId++;
        m_scheduled.push_back({id, seconds, 0.0f, false, false, std::move(fn)});
        return id;
    });
    m_lua.set_function("Repeat", [this](float intervalSeconds, sol::protected_function fn) -> int {
        int id = m_nextTimerId++;
        m_scheduled.push_back({id, intervalSeconds, intervalSeconds, true, false, std::move(fn)});
        return id;
    });
    m_lua.set_function("CancelTimer", [this](int id) {
        for (auto& call : m_scheduled) {
            if (call.Id == id) { call.Cancelled = true; break; }
        }
    });

    // --- Корутины: последовательности во времени как линейный код —
    // StartCoroutine(function() ... wait(1.0) ... end) вместо ручного
    // стейт-машины из Schedule-вызовов. wait() определён Lua-обвязкой ниже,
    // это просто именованная обёртка над coroutine.yield для читаемости.
    //
    // ВАЖНО: sol::coroutine должен строиться НАПРЯМУЮ из sol::function —
    // sol2 сам создаёт для неё новый Lua-поток (thread) с правильной
    // внутренней настройкой стека вызова. Если вместо этого передать сюда
    // результат ручного coroutine.create(fn), первый же resume падает с
    // "attempt to call a thread value" — sol2 ожидает управлять созданием
    // потока сам, а не оборачивать уже готовый Lua-thread.
    m_lua.set_function("StartCoroutine", [this](sol::function fn) {
        // sol::coroutine, построенный НАПРЯМУЮ из функции главного Lua-состояния
        // (sol::coroutine co = fn;), не создаёт для неё отдельный Lua-поток —
        // lua_resume() в его реализации вызывается на lua_state() САМОЙ fn,
        // то есть на главном состоянии. Пока активна только ОДНА такая
        // "корутина", это незаметно работает случайно; как только их две
        // одновременно, вторая резюмится на ТОМ ЖЕ lua_State*, что и первая,
        // и по факту продолжает выполнение первой вместо своей функции.
        // Поэтому явно создаём отдельный Lua-поток (sol::thread) и переносим
        // на его стек функцию через lua_xmove перед тем, как обернуть в
        // sol::coroutine — так каждая корутина резюмится на СВОЁМ потоке.
        sol::thread runner = sol::thread::create(m_lua.lua_state());
        lua_State* runnerState = runner.state().lua_state();
        fn.push();
        lua_xmove(fn.lua_state(), runnerState, 1);
        sol::coroutine co(runnerState, -1);
        m_coroutines.push_back({std::move(co), 0.0f, std::move(runner)});
    });

    sol::protected_function_result bootstrap = m_lua.script(
        "function wait(seconds) return coroutine.yield(seconds or 0) end",
        sol::script_pass_on_error);
    if (!bootstrap.valid()) {
        sol::error err = bootstrap;
        LOG_ERROR("ScriptEngine") << "Не удалось зарегистрировать встроенную функцию wait(): " << err.what();
    }
}

void ScriptEngine::AttachScript(GameObject& object, const std::string& scriptPath) {
    sol::environment env(m_lua, sol::create, m_lua.globals());

    auto result = m_lua.script_file(scriptPath, env, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        throw std::runtime_error("Ошибка загрузки скрипта " + scriptPath + ": " + err.what());
    }

    sol::protected_function updateFn = env["OnUpdate"];
    // OnUpdate необязателен для объектных скриптов, которые всё делают в
    // OnStart или через StartCoroutine/Schedule — раньше это было required,
    // что мешало писать скрипты "разово настроил и забыл".

    sol::protected_function startFn = env["OnStart"];
    if (startFn.valid()) {
        auto startResult = startFn(object);
        if (!startResult.valid()) {
            sol::error err = startResult;
            LOG_ERROR("ScriptEngine") << "Ошибка в OnStart (" << scriptPath << "): " << err.what();
        }
    }

    m_instances.push_back({ &object, std::move(env), std::move(updateFn), scriptPath });
}

void ScriptEngine::RunScript(const std::string& scriptPath) {
    sol::environment env(m_lua, sol::create, m_lua.globals());

    auto result = m_lua.script_file(scriptPath, env, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        throw std::runtime_error("Ошибка загрузки скрипта " + scriptPath + ": " + err.what());
    }

    sol::protected_function updateFn = env["OnUpdate"];

    sol::protected_function startFn = env["OnStart"];
    if (startFn.valid()) {
        auto startResult = startFn();
        if (!startResult.valid()) {
            sol::error err = startResult;
            LOG_ERROR("ScriptEngine") << "Ошибка в OnStart (" << scriptPath << "): " << err.what();
        }
    }

    m_instances.push_back({ nullptr, std::move(env), std::move(updateFn), scriptPath });
}

void ScriptEngine::UpdateAll(float deltaTime) {
    for (auto& instance : m_instances) {
        if (instance.Dead) continue; // Object уничтожен через DestroyObject() — см. ScriptInstance::Dead
        if (!instance.UpdateFn.valid()) continue; // скрипт без OnUpdate — легитимно (см. AttachScript)

        // Объектные скрипты получают entity первым аргументом, уровневые — нет
        auto result = instance.Object ? instance.UpdateFn(*instance.Object, deltaTime)
                                       : instance.UpdateFn(deltaTime);
        if (!result.valid()) {
            sol::error err = result;
            std::string who = instance.Object ? instance.Object->Name : instance.Path;
            LOG_ERROR("ScriptEngine") << "Ошибка в OnUpdate (" << who << "): " << err.what();
        }
    }

    // Убираем экземпляры с уничтоженным Object одним проходом ПОСЛЕ основного
    // цикла — как и ниже для таймеров/корутин, чтобы не мутировать m_instances
    // во время его же итерации (в т.ч. если DestroyObject был вызван изнутри
    // самого OnUpdate этого же экземпляра).
    m_instances.erase(
        std::remove_if(m_instances.begin(), m_instances.end(), [](const ScriptInstance& i) { return i.Dead; }),
        m_instances.end());

    UpdateTimers(deltaTime);
    UpdateCoroutines(deltaTime);
    UpdatePendingMeshes();
}

// Назначает объектам меши, чья асинхронная загрузка завершилась (см.
// SetMeshModelAsync). Готовые/ошибочные/осиротевшие записи убираются.
void ScriptEngine::UpdatePendingMeshes() {
    for (auto& pending : m_pendingMeshes) {
        if (pending.Handle.IsFailed()) {
            LOG_WARN("ScriptEngine") << "Async-модель не загрузилась: " << pending.Path
                                     << " (" << pending.Handle.Error() << ")";
            continue;
        }
        if (!pending.Handle.IsReady()) continue; // ещё грузится
        if (GameObject* obj = m_scene ? m_scene->FindById(pending.ObjectId) : nullptr) {
            obj->MeshComponent = pending.Handle.Shared();
        }
    }
    m_pendingMeshes.erase(
        std::remove_if(m_pendingMeshes.begin(), m_pendingMeshes.end(),
                       [](const PendingMeshLoad& p) { return !p.Handle.IsLoading(); }),
        m_pendingMeshes.end());
}

void ScriptEngine::UpdateTimers(float dt) {
    // Индекс, а не диапазон/итератор: колбэк таймера может сам вызвать
    // Schedule/Repeat (см. ниже), чей push_back способен реаллоцировать
    // m_scheduled — любой ранее взятый итератор/ссылка внутрь вектора после
    // этого висячий (undefined behavior). Индекс остаётся корректным для уже
    // пройденных и текущего элемента (до конца этой функции ничего не
    // удаляется поэлементно, только один проход erase-remove в самом конце).
    //
    // Сам колбэк ПЕРЕД вызовом копируем в локальную переменную, а не вызываем
    // прямо "m_scheduled[i].Fn()": sol2 после resume пишет результат обратно
    // в себя (в "this"), и если бы этот "this" жил внутри вектора, реаллокация
    // ВНУТРИ самого вызова (из-за вложенного Schedule/Repeat) оборвала бы его
    // раньше, чем вызов успеет завершиться — copy sol::protected_function
    // дешёвый (просто ссылка на тот же Lua-объект), поэтому это не накладно.
    for (size_t i = 0; i < m_scheduled.size(); ++i) {
        if (m_scheduled[i].Cancelled) continue;
        m_scheduled[i].TimeLeft -= dt;
        if (m_scheduled[i].TimeLeft > 0.0f) continue;

        int id = m_scheduled[i].Id;
        sol::protected_function fn = m_scheduled[i].Fn;

        auto result = fn();
        if (!result.valid()) {
            sol::error err = result;
            LOG_ERROR("ScriptEngine") << "Ошибка в таймере (id " << id << "): " << err.what();
        }

        // m_scheduled могла реаллоцироваться внутри fn() — обращаемся к
        // элементу заново по тому же индексу, не через старую ссылку.
        if (m_scheduled[i].Repeating) {
            m_scheduled[i].TimeLeft += m_scheduled[i].Interval; // "+=", не "=", чтобы не копить дрейф при просадках FPS
        } else {
            m_scheduled[i].Cancelled = true; // одноразовый — гасим после первого срабатывания
        }
    }

    // Чистим отменённые/сработавшие одноразовые вызовы одним проходом за кадр
    m_scheduled.erase(
        std::remove_if(m_scheduled.begin(), m_scheduled.end(), [](const ScheduledCall& c) { return c.Cancelled; }),
        m_scheduled.end());
}

void ScriptEngine::UpdateCoroutines(float dt) {
    // Индекс, а не итератор — по той же причине, что и в UpdateTimers: тело
    // корутины может само вызвать StartCoroutine, чей push_back способен
    // реаллоцировать m_coroutines. Резюмируем ЛОКАЛЬНУЮ КОПИЮ sol::coroutine
    // (дешёвая операция — копирует лишь ссылку на тот же Lua-поток), а не
    // объект, живущий прямо в векторе: sol2 после lua_resume пишет статус
    // обратно в себя (в "this"), и если бы "this" был указателем внутрь
    // вектора, реаллокация ВНУТРИ самого вызова оборвала бы его раньше, чем
    // вызов успеет завершиться. Индекс остаётся корректным после реаллокации
    // (erase здесь — только для текущего/уже пройденных элементов).
    for (size_t i = 0; i < m_coroutines.size(); ) {
        m_coroutines[i].WaitTime -= dt;
        if (m_coroutines[i].WaitTime > 0.0f) { ++i; continue; }

        sol::coroutine co = m_coroutines[i].Co;
        auto result = co();

        if (!result.valid()) {
            sol::error err = result;
            LOG_ERROR("ScriptEngine") << "Ошибка в корутине: " << err.what();
            m_coroutines.erase(m_coroutines.begin() + i);
            continue;
        }

        if (co.status() == sol::call_status::yielded) {
            // wait(seconds) вернул через yield время следующей паузы.
            // m_coroutines могла реаллоцироваться внутри co() — обращаемся к
            // элементу заново по тому же индексу, не через старую ссылку.
            float nextWait = result.get<sol::optional<float>>().value_or(0.0f);
            m_coroutines[i].Co = co;
            m_coroutines[i].WaitTime = nextWait;
            ++i;
        } else {
            // Корутина дошла до конца функции — больше резюмировать нечего
            m_coroutines.erase(m_coroutines.begin() + i);
        }
    }
}

const Texture* ScriptEngine::GetOrLoadBillboardTexture(const std::string& path) {
    auto it = m_billboardTextures.find(path);
    if (it != m_billboardTextures.end()) return it->second.Get();

    // Грузим через единую систему ассетов (кэш/дедуп/hot-reload — общие).
    // Bilinear — разумный дефолт для одиночных спрайтов (не атлас, в отличие
    // от блочного атласа вокселей, которому нужен Nearest). Держим хендл
    // Asset<Texture> здесь: билборд хранит невладеющий указатель, а хендл не
    // даёт AssetManager'у выгрузить текстуру, пока жив ScriptEngine.
    Asset<Texture> tex = AssetManager::Instance().LoadTexture(path, TextureFilter::Bilinear);
    if (!tex.IsReady()) return nullptr;
    const Texture* raw = tex.Get();
    m_billboardTextures[path] = tex;
    return raw;
}

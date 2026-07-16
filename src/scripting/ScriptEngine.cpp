#include "ScriptEngine.h"
#include "../core/Log.h"
#include "../asset/AssetManager.h"
#include "../render/ParticlePresets.h"
#include <algorithm>
#include <any>

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

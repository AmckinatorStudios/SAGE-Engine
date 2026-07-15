#include "ScriptEngine.h"
#include "../core/Log.h"
#include "../render/ResourceManager.h"
#include <algorithm>

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
        "Color", &GameObject::Color
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
        m_scene->RemoveObject(id);
    });

    // Удобный хелпер: даёт заспавненному объекту видимый куб-меш, чтобы его
    // сразу было видно на сцене без ручной возни с MeshRef/ResourceManager
    m_lua.set_function("SetMeshCube", [](GameObject& obj) {
        obj.MeshRefComponent = MeshRef{MeshRef::Type::Cube, ""};
        obj.MeshComponent = ResourceManager::Instance().GetCube();
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
        sol::coroutine co = fn;
        m_coroutines.push_back({std::move(co), 0.0f});
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

    UpdateTimers(deltaTime);
    UpdateCoroutines(deltaTime);
}

void ScriptEngine::UpdateTimers(float dt) {
    for (auto& call : m_scheduled) {
        if (call.Cancelled) continue;
        call.TimeLeft -= dt;
        if (call.TimeLeft > 0.0f) continue;

        auto result = call.Fn();
        if (!result.valid()) {
            sol::error err = result;
            LOG_ERROR("ScriptEngine") << "Ошибка в таймере (id " << call.Id << "): " << err.what();
        }

        if (call.Repeating) {
            call.TimeLeft += call.Interval; // "+=", не "=", чтобы не копить дрейф при просадках FPS
        } else {
            call.Cancelled = true; // одноразовый — гасим после первого срабатывания
        }
    }

    // Чистим отменённые/сработавшие одноразовые вызовы одним проходом за кадр
    m_scheduled.erase(
        std::remove_if(m_scheduled.begin(), m_scheduled.end(), [](const ScheduledCall& c) { return c.Cancelled; }),
        m_scheduled.end());
}

void ScriptEngine::UpdateCoroutines(float dt) {
    for (auto it = m_coroutines.begin(); it != m_coroutines.end(); ) {
        it->WaitTime -= dt;
        if (it->WaitTime > 0.0f) { ++it; continue; }

        auto result = it->Co();
        if (!result.valid()) {
            sol::error err = result;
            LOG_ERROR("ScriptEngine") << "Ошибка в корутине: " << err.what();
            it = m_coroutines.erase(it);
            continue;
        }

        if (it->Co.status() == sol::call_status::yielded) {
            // wait(seconds) вернул через yield время следующей паузы
            float nextWait = result.get<sol::optional<float>>().value_or(0.0f);
            it->WaitTime = nextWait;
            ++it;
        } else {
            // Корутина дошла до конца функции — больше резюмировать нечего
            it = m_coroutines.erase(it);
        }
    }
}

#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/physics/Ragdoll.h"

// ---------------------------------------------------------------------------
// Физика: sage.physics.*
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterPhysicsApi() {
    // --- Физика времени выполнения: доступна после BindPhysics. Управляет
    // линейной скоростью тела сущности (у неё должен быть RigidBodyComponent,
    // и симуляция должна идти) и гравитацией всего мира. Если тела нет/оно ещё
    // не построено — тихий no-op / нулевая скорость (не ошибка). ---
    Bind("physics", "SetVelocity", "SetVelocity", [this](GameObject& obj, const glm::vec3& v) {
        if (!m_physics) throw std::runtime_error("SetVelocity: физика не привязана (BindPhysics не вызван)");
        if (!obj.Valid()) return;
        auto* rb = obj.Registry()->try_get<RigidBodyComponent>(obj.Entity());
        if (!rb || rb->RuntimeBody == sage::physics::kInvalidBody) return;
        m_physics->SetLinearVelocity(rb->RuntimeBody, v);
    });
    Bind("physics", "GetVelocity", "GetVelocity", [this](GameObject& obj) -> glm::vec3 {
        if (!m_physics) throw std::runtime_error("GetVelocity: физика не привязана (BindPhysics не вызван)");
        if (!obj.Valid()) return glm::vec3(0.0f);
        auto* rb = obj.Registry()->try_get<RigidBodyComponent>(obj.Entity());
        if (!rb || rb->RuntimeBody == sage::physics::kInvalidBody) return glm::vec3(0.0f);
        return m_physics->GetLinearVelocity(rb->RuntimeBody);
    });
    // --- Запросы к физическому миру ----------------------------------------
    //
    // Луч — то, без чего игра пишется в обход движка. Пока его не было,
    // «выстрелить», «подобрать прицелом» и «узнать высоту пола» приходилось
    // считать в Lua по собственной геометрии — а система IK прямо требовала
    // высоту опоры ОТ ИГРЫ, потому что спросить было не у кого.
    //
    // Возвращает таблицу {object, point, normal, distance} или nil. Таблица, а
    // не набор возвращаемых значений: у промаха тогда один честный ответ (nil),
    // а не четыре нуля, которые легко принять за попадание в начало координат.
    Bind("physics", "Raycast", "Raycast", [this](const glm::vec3& origin, const glm::vec3& dir,
                                         float maxDistance,
                                         sol::optional<unsigned> mask) -> sol::object {
        if (!m_physics) throw std::runtime_error("Raycast: физика не привязана (BindPhysics не вызван)");
        const PhysicsScene::EntityHit hit = m_physics->Raycast(
            origin, dir, maxDistance, mask.value_or(sage::physics::kAllLayers));
        if (!hit.Hit) return sol::nil;
        sol::table t = m_lua.create_table();
        t["object"] = GameObject(&m_scene->Registry(), hit.Entity);
        t["point"] = hit.Point;
        t["normal"] = hit.Normal;
        t["distance"] = hit.Distance;
        return t;
    });

    // Все объекты в шаре: взрыв, зона подбора, «кто рядом».
    Bind("physics", "OverlapSphere", "OverlapSphere", [this](const glm::vec3& center, float radius,
                                               sol::optional<unsigned> mask) -> sol::table {
        sol::table list = m_lua.create_table();
        if (!m_physics) return list;
        std::vector<entt::entity> found;
        m_physics->OverlapSphere(center, radius, found, mask.value_or(sage::physics::kAllLayers));
        for (size_t i = 0; i < found.size(); ++i) list[i + 1] = GameObject(&m_scene->Registry(), found[i]);
        return list;
    });

    // Столкновения за последний шаг. Опрос, а не колбэк на каждое событие:
    // физика зовёт свои обработчики из рабочих потоков, а Lua однопоточен —
    // и вызывать оттуда скрипт нельзя ни при каких условиях.
    Bind("physics", "Collisions", "Collisions", [this]() -> sol::table {
        sol::table list = m_lua.create_table();
        if (!m_physics) return list;
        int i = 1;
        for (const PhysicsScene::EntityContact& c : m_physics->Contacts()) {
            sol::table t = m_lua.create_table();
            t["a"] = GameObject(&m_scene->Registry(), c.A);
            t["b"] = GameObject(&m_scene->Registry(), c.B);
            t["began"] = c.Begin;
            t["point"] = c.Point;
            t["normal"] = c.Normal;
            t["impulse"] = c.Impulse;
            t["sensor"] = c.Sensor;
            list[i++] = t;
        }
        return list;
    });

    // --- Контроллер персонажа ----------------------------------------------
    //
    // Скорость ЗАДАЁТСЯ каждый кадр, а не накапливается: тяготение, прыжок и
    // ускорение — правила игры, а не физики. Движок отвечает только за то, что
    // персонаж не пройдёт сквозь стену, взойдёт на ступеньку и удержится на
    // склоне.
    Bind("physics", "AddCharacter", "AddCharacterController", [](GameObject& obj, sol::optional<float> radius,
                                                    sol::optional<float> height) {
        if (!obj.Valid()) return;
        auto& cc = obj.Registry()->get_or_emplace<CharacterControllerComponent>(obj.Entity());
        if (radius) cc.Radius = *radius;
        if (height) cc.Height = *height;
    });
    Bind("physics", "MoveCharacter", "MoveCharacter", [this](GameObject& obj, const glm::vec3& velocity,
                                               float dt) {
        if (!obj.Valid() || !m_physics) return;
        auto* cc = obj.Registry()->try_get<CharacterControllerComponent>(obj.Entity());
        if (!cc || cc->Runtime == sage::physics::kInvalidCharacter) return;
        m_physics->MoveCharacter(cc->Runtime, velocity, dt);
    });
    // Стоит ли персонаж на опоре. По этому флагу игра решает, можно ли прыгать
    // и не пора ли играть шаги — без него приходится гадать по скорости, а
    // скорость на склоне обманывает.
    Bind("physics", "IsGrounded", "IsGrounded", [](GameObject& obj) -> bool {
        if (!obj.Valid()) return false;
        auto* cc = obj.Registry()->try_get<CharacterControllerComponent>(obj.Entity());
        return cc && cc->Grounded;
    });
    Bind("physics", "GroundNormal", "GroundNormal", [](GameObject& obj) -> glm::vec3 {
        if (!obj.Valid()) return glm::vec3(0.0f, 1.0f, 0.0f);
        auto* cc = obj.Registry()->try_get<CharacterControllerComponent>(obj.Entity());
        return cc ? cc->GroundNormal : glm::vec3(0.0f, 1.0f, 0.0f);
    });
    // Телепорт: перенос персонажа мимо симуляции (респавн, переход по уровню).
    Bind("physics", "SetCharacterPosition", "SetCharacterPosition", [this](GameObject& obj, const glm::vec3& pos) {
        if (!obj.Valid() || !m_physics) return;
        auto* cc = obj.Registry()->try_get<CharacterControllerComponent>(obj.Entity());
        if (cc && cc->Runtime != sage::physics::kInvalidCharacter)
            m_physics->SetCharacterPosition(cc->Runtime, pos);
        if (auto* tr = obj.Registry()->try_get<Transform>(obj.Entity())) tr->Position = pos;
    });
    // Слой и сенсорность тела — из скрипта, чтобы игра раздавала слои сама.
    Bind("physics", "SetLayer", "SetPhysicsLayer", [](GameObject& obj, unsigned layer) {
        if (!obj.Valid()) return;
        if (auto* rb = obj.Registry()->try_get<RigidBodyComponent>(obj.Entity())) {
            rb->Layer = layer;
            rb->RuntimeBody = sage::physics::kInvalidBody;  // пересоздать с новым слоем
        }
    });
    Bind("physics", "SetSensor", "SetSensor", [](GameObject& obj, bool on) {
        if (!obj.Valid()) return;
        if (auto* rb = obj.Registry()->try_get<RigidBodyComponent>(obj.Entity())) {
            rb->Sensor = on;
            rb->RuntimeBody = sage::physics::kInvalidBody;
        }
    });

    Bind("physics", "SetGravity", "SetGravity", [this](const glm::vec3& g) {
        if (!m_physics) throw std::runtime_error("SetGravity: физика не привязана (BindPhysics не вызван)");
        m_physics->SetGravity(g);
    });
    // Мгновенный импульс в тело сущности (толчок/пинок/выстрел). No-op, если у
    // сущности нет тела или симуляция не идёт.
    Bind("physics", "AddImpulse", "AddImpulse", [this](GameObject& obj, const glm::vec3& impulse) {
        if (!m_physics) throw std::runtime_error("AddImpulse: физика не привязана (BindPhysics не вызван)");
        if (!obj.Valid()) return;
        auto* rb = obj.Registry()->try_get<RigidBodyComponent>(obj.Entity());
        if (!rb || rb->RuntimeBody == sage::physics::kInvalidBody) return;
        m_physics->AddImpulse(rb->RuntimeBody, impulse);
    });
    // Собирает тряпичную куклу (кости-капсулы + суставы) в сцене на месте pos.
    // Возвращает id корневой сущности (таз). Полноценно симулируется на Jolt;
    // на Simple кости просто падают по отдельности (joints не поддержаны).
    Bind("scene", "SpawnRagdoll", "SpawnRagdoll", [this](const glm::vec3& pos, sol::optional<float> scale) -> int {
        if (!m_scene) throw std::runtime_error("SpawnRagdoll: сцена не привязана");
        return sage::physics::BuildRagdoll(*m_scene, pos, scale.value_or(1.0f));
    });
}


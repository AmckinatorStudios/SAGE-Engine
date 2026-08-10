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
    // Размеры и повадки контроллера из скрипта. Без этого рост, радиус и
    // особенно ВЫСОТА СТУПЕНЬКИ настраивались только из C++ — то есть игра
    // целиком на Lua настроить их не могла вовсе и была обречена писать ходьбу
    // сама.
    Bind("physics", "SetCharacterShape", "SetCharacterShape", [](GameObject& obj, sol::table t) {
        if (!obj.Valid()) return;
        auto& cc = obj.Registry()->get_or_emplace<CharacterControllerComponent>(obj.Entity());
        cc.Radius = t.get_or("radius", cc.Radius);
        cc.Height = t.get_or("height", cc.Height);
        cc.StepHeight = t.get_or("step", cc.StepHeight);
        cc.MaxSlopeDeg = t.get_or("slope", cc.MaxSlopeDeg);
        cc.Mass = t.get_or("mass", cc.Mass);
        // Форма поменялась — контроллер пересоздать. И у бэкенда, и у мотора:
        // иначе новая высота ступеньки вступила бы в силу только после
        // перезапуска, и настройка выглядела бы неработающей.
        cc.Runtime = sage::physics::kInvalidCharacter;
        if (cc.Motor) {
            sage::physics::CharacterDesc d;
            d.Radius = cc.Radius; d.Height = cc.Height; d.StepHeight = cc.StepHeight;
            d.MaxSlopeDeg = cc.MaxSlopeDeg; d.Mass = cc.Mass; d.Layer = cc.Layer;
            d.Position = cc.Motor->State().Position;
            cc.Motor->Configure(d);
        }
    });

    // --- СВОЙ МИР персонажа -------------------------------------------------
    //
    // Игра отдаёт функцию «занят ли объём», и контроллер начинает ходить по её
    // геометрии: по воксельной сетке, собранной скриптом, по палубе в
    // координатах качающегося корпуса, по чему угодно, чего нет в физике.
    //
    // Зачем это движку. Контроллер персонажа был заперт внутри физического
    // бэкенда, и игра, чей мир в физике не лежит, не могла им воспользоваться
    // в принципе — она писала свою ходьбу и наживала свои ошибки. Причём
    // одни и те же: почти каждая самодельная ходьба рано или поздно позволяет
    // взбираться по отвесу «ступеньками». Здесь алгоритм один на всех и
    // проверен тестами (см. CharacterMotor).
    //
    // Колбэк получает ШЕСТЬ ЧИСЕЛ, а не таблицу с векторами: он зовётся
    // десятки раз за кадр (полуделение по трём осям, подшаги, попытка
    // ступеньки), и таблица на каждый вызов — это мусор для сборщика на ровном
    // месте.
    Bind("physics", "SetCharacterWorld", "SetCharacterWorld", [this](GameObject& obj,
                                                                     sol::object fn) {
        if (!obj.Valid()) return;
        auto& cc = obj.Registry()->get_or_emplace<CharacterControllerComponent>(obj.Entity());
        if (!fn.valid() || fn == sol::nil) {
            cc.Solid = nullptr;
            cc.Motor.reset();
            return;
        }
        sol::protected_function query = fn.as<sol::protected_function>();
        // Жалуемся ОДИН раз на контроллер: ошибка внутри запроса повторяется на
        // каждую пробу, то есть десятки раз за кадр — в таком потоке не видно
        // ни первой строки, ни чего-либо ещё.
        auto warned = std::make_shared<bool>(false);
        cc.Solid = [query, warned](const glm::vec3& min, const glm::vec3& max) -> bool {
            sol::protected_function_result r = query(min.x, min.y, min.z, max.x, max.y, max.z);
            if (!r.valid()) {
                if (!*warned) {
                    *warned = true;
                    const sol::error err = r;
                    LOG_ERROR("ScriptEngine")
                        << "SetCharacterWorld: ошибка в запросе тверди: " << err.what();
                }
                // Считаем объём занятым: персонаж остановится там, где стоял.
                // Обратное решение уронило бы его сквозь мир — и виноватой
                // выглядела бы физика, а не сломанный скрипт.
                return true;
            }
            return r.get<bool>();
        };
        if (!cc.Motor) cc.Motor = std::make_shared<sage::physics::CharacterMotor>();
        sage::physics::CharacterDesc d;
        d.Radius = cc.Radius; d.Height = cc.Height; d.StepHeight = cc.StepHeight;
        d.MaxSlopeDeg = cc.MaxSlopeDeg; d.Mass = cc.Mass; d.Layer = cc.Layer;
        if (auto* tr = obj.Registry()->try_get<Transform>(obj.Entity())) d.Position = tr->Position;
        cc.Motor->Configure(d);
    });

    Bind("physics", "MoveCharacter", "MoveCharacter", [this](GameObject& obj, const glm::vec3& velocity,
                                               float dt) {
        if (!obj.Valid()) return;
        auto* cc = obj.Registry()->try_get<CharacterControllerComponent>(obj.Entity());
        if (!cc) return;
        if (cc->Motor) {
            // Свой мир: шагаем мотором и сразу же переносим результат в
            // Transform и в поля компонента. Ждать PullCharacters нельзя —
            // физика об этом персонаже не знает и не тронет его никогда.
            cc->Motor->Move(cc->Solid, velocity, dt);
            const sage::physics::CharacterState& st = cc->Motor->State();
            if (auto* tr = obj.Registry()->try_get<Transform>(obj.Entity())) tr->Position = st.Position;
            cc->Grounded = st.Grounded;
            cc->GroundNormal = st.GroundNormal;
            cc->Landed = cc->Motor->Landed();
            cc->LeftGround = cc->Motor->LeftGround();
            cc->Blocked = cc->Motor->Blocked();
            cc->StepUp = cc->Motor->StepUp();
            return;
        }
        if (!m_physics || cc->Runtime == sage::physics::kInvalidCharacter) return;
        m_physics->MoveCharacter(cc->Runtime, velocity, dt);
    });

    // Всё состояние контроллера одной таблицей: позиция, скорость, опора и
    // КРАЯ последнего шага. Края здесь не для удобства — «приземлился» и
    // «взошёл на ступеньку» игра иначе вычисляет сравнением флагов между
    // кадрами и теряет их на длинном кадре, ровно тогда, когда просадка и
    // без того портит впечатление.
    Bind("physics", "CharacterState", "CharacterState", [this](GameObject& obj) -> sol::object {
        if (!obj.Valid()) return sol::nil;
        auto* cc = obj.Registry()->try_get<CharacterControllerComponent>(obj.Entity());
        if (!cc) return sol::nil;
        sage::physics::CharacterState st;
        if (cc->Motor) st = cc->Motor->State();
        else if (m_physics && cc->Runtime != sage::physics::kInvalidCharacter)
            st = m_physics->CharacterState(cc->Runtime);
        sol::table t = m_lua.create_table();
        t["position"] = st.Position;
        t["velocity"] = st.Velocity;
        t["grounded"] = cc->Motor ? st.Grounded : cc->Grounded;
        t["normal"] = st.GroundNormal;
        t["landed"] = cc->Landed;
        t["leftGround"] = cc->LeftGround;
        t["blocked"] = cc->Blocked;
        t["stepUp"] = cc->StepUp;
        return t;
    });

    // Влезет ли персонаж подошвами в эту точку — спрашивают ДО телепорта.
    // Перенос внутрь стены оставляет его замурованным, и выбирается он оттуда
    // рывком вверх на глазах у игрока.
    Bind("physics", "CharacterFits", "CharacterFits", [](GameObject& obj, const glm::vec3& feet) -> bool {
        if (!obj.Valid()) return true;
        auto* cc = obj.Registry()->try_get<CharacterControllerComponent>(obj.Entity());
        if (!cc || !cc->Motor) return true;
        return cc->Motor->Fits(cc->Solid, feet);
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
    // встроенный бэкенд держит суставы так же — оба умеют соединения.
    Bind("scene", "SpawnRagdoll", "SpawnRagdoll", [this](const glm::vec3& pos, sol::optional<float> scale) -> int {
        if (!m_scene) throw std::runtime_error("SpawnRagdoll: сцена не привязана");
        return sage::physics::BuildRagdoll(*m_scene, pos, scale.value_or(1.0f));
    });
}


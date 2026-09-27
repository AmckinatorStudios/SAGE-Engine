#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/render/RenderTexture.h"

// ---------------------------------------------------------------------------
// События: sage.events.*
//
// Шина «подписался по имени — получил вызов». До неё скрипты узнавали о
// происходящем опросом каждый кадр (самодельный детектор края теряет событие
// на длинном кадре) или сообщениями, адресованными сущностям — уровневому
// скрипту они недоступны.
//
// Обработчики зовутся сразу из Emit, не через очередь: движок никогда не шлёт
// события из чужого потока (физика отдаёт контакты опросом на главном), а
// очередь добавила бы каждому событию кадр задержки.
//
// ВСТРОЕННЫЕ СОБЫТИЯ (движок шлёт их сам, игре достаточно подписаться):
//   "collision"        {a, b, began, point, normal, impulse}   — удар тел
//   "trigger"          {a, b, entered, point}                  — зона-сенсор
//   "character.land"   {object, velocity}                      — приземлился
//   "character.leave"  {object}                                — оторвался от опоры
//   "character.step"   {object, height}                        — взошёл на ступеньку
//   "character.blocked"{object}                                — упёрся в стену
//   "action.pressed"   {action}                                — нажато действие ввода
//   "action.released"  {action}
//   "pause"            {paused}
//   "quit"             {}
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterEventsApi() {
    // ЭТО НЕ ОТДЕЛЬНАЯ ШИНА. sage.events — старое имя глобальных событий, и
    // подписка отсюда заводится в той же шине сцены (объект 0), что и
    // Events.on нового API и связи инспектора: событие, посланное кнопкой, C++
    // или новым скриптом, слышно здесь, и наоборот.
    //
    // Подписка возвращает номер соединения: возвращать саму функцию нельзя —
    // Lua сравнивает замыкания по идентичности, и снять подписку на
    // `function() ... end`, объявленную по месту, было бы уже невозможно.
    auto need = [](const std::string& who, const std::string& name, const sol::protected_function& fn) {
        if (name.empty()) throw std::runtime_error(who + ": пустое имя события");
        if (!fn.valid()) throw std::runtime_error(who + "('" + name + "'): обработчик не функция");
    };
    Bind("events", "On", "OnEvent", [this, need](const std::string& name, sol::protected_function fn) -> int {
        need("sage.events.On", name, fn);
        return m_events->Connect(EventBus(), sage::events::Bus::kGlobal, name, fn, false, 0);
    });

    // Одноразовая подписка: сработала — снялась.
    Bind("events", "Once", "OnceEvent", [this, need](const std::string& name, sol::protected_function fn) -> int {
        need("sage.events.Once", name, fn);
        return m_events->Connect(EventBus(), sage::events::Bus::kGlobal, name, fn, true, 0);
    });

    Bind("events", "Off", "OffEvent", [this](sol::object arg) {
        // Снять можно по номеру подписки или по имени события целиком (только
        // свои подписки: обработчики C++ и связи инспектора — не наши).
        if (arg.is<int>()) {
            EventBus().Disconnect(arg.as<int>());
            return;
        }
        if (arg.is<std::string>())
            EventBus().DisconnectSignal(sage::events::Bus::kGlobal, arg.as<std::string>(),
                                        m_events->Group());
    });

    // Рассылка. Полезная нагрузка — одно значение (обычно таблица): набор
    // аргументов переменной длины заставил бы каждого подписчика знать
    // порядок и число полей отправителя. Таблица едет подписчикам Lua КАК ЕСТЬ
    // (Event::Payload), а код на C++ читает её значение в Arg.
    Bind("events", "Emit", "EmitEvent", [this](const std::string& name, sol::object payload) {
        if (name.empty()) throw std::runtime_error("sage.events.Emit: пустое имя события");
        DispatchEvent(name, payload);
    });

    // Сколько живых подписчиков у события. Нужно не для отладки: рассылка
    // тяжёлой нагрузки имеет смысл только если её кто-то слушает.
    Bind("events", "Count", "EventCount", [this](const std::string& name) -> int {
        return EventBus().Count(name);
    });
}

// Рассылка одного глобального события. Снимок подписчиков, защита от
// рассылки по кругу и снятие одноразовых — у шины (sage/events/Events.cpp):
// второй копии этих правил здесь больше нет.
void ScriptEngine::DispatchEvent(const std::string& name, sol::object payload) {
    sage::events::Event e;
    e.Name = name;
    e.Arg = ValueFromLua(payload);
    if (payload.valid() && payload.get_type() != sol::type::lua_nil) e.Payload = payload;
    EventBus().Emit(e);
}

void ScriptEngine::DispatchEvent(const std::string& name, bool flag) {
    sol::table t = m_lua.create_table();
    t["value"] = flag;
    DispatchEvent(name, t);
}

// --- Встроенные события кадра -------------------------------------------------
//
// Зовётся хозяином кадра ПОСЛЕ физики и ввода, но ДО пользовательских OnUpdate:
// скрипт, обрабатывающий столкновение, должен успеть отреагировать в том же
// кадре, в котором оно случилось, — иначе взрыв отстаёт от удара на кадр, и
// это видно.
void ScriptEngine::DispatchFrameEvents() {
    if (!m_scene) return;

    // 1. Столкновения и зоны-сенсоры.
    if (m_physics) {
        for (const PhysicsScene::EntityContact& c : m_physics->Contacts()) {
            const char* name = c.Sensor ? "trigger" : "collision";
            if (EventBus().Count(name) == 0) continue;
            sol::table t = m_lua.create_table();
            t["a"] = GameObject(&m_scene->Registry(), c.A);
            t["b"] = GameObject(&m_scene->Registry(), c.B);
            if (c.Sensor) t["entered"] = c.Begin;
            else t["began"] = c.Begin;
            t["point"] = c.Point;
            t["normal"] = c.Normal;
            t["impulse"] = c.Impulse;
            DispatchEvent(name, t);
        }
    }

    // 2. Края контроллера персонажа. Их считает мотор (см. CharacterMotor):
    // «приземлился» и «взошёл на ступеньку» — края, а край, вычисленный
    // снаружи опросом раз в кадр, теряется ровно на длинном кадре, когда
    // просадка и без того портит впечатление.
    auto view = m_scene->Registry().view<CharacterControllerComponent>();
    for (auto e : view) {
        const CharacterControllerComponent& cc = view.get<CharacterControllerComponent>(e);
        if (!cc.Landed && !cc.LeftGround && !cc.Blocked && cc.StepUp <= 0.0f) continue;
        GameObject obj(&m_scene->Registry(), e);
        if (cc.Landed) {
            sol::table t = m_lua.create_table();
            t["object"] = obj;
            DispatchEvent("character.land", t);
        }
        if (cc.LeftGround) {
            sol::table t = m_lua.create_table();
            t["object"] = obj;
            DispatchEvent("character.leave", t);
        }
        if (cc.StepUp > 0.0f) {
            sol::table t = m_lua.create_table();
            t["object"] = obj;
            t["height"] = cc.StepUp;
            DispatchEvent("character.step", t);
        }
        if (cc.Blocked) {
            sol::table t = m_lua.create_table();
            t["object"] = obj;
            DispatchEvent("character.blocked", t);
        }
    }

    // 3. Именованные действия ввода. Событием, а не опросом: «нажал» — это
    // край, и на просадке WasActionPressed его теряет так же, как всё
    // остальное.
    if (m_input) {
        const bool wantPressed = EventBus().Count("action.pressed") != 0;
        const bool wantReleased = EventBus().Count("action.released") != 0;
        if (wantPressed || wantReleased) {
            // По всем контекстам, а не только по игровому: действие, объявленное
            // в контексте меню, обязано доходить до скрипта так же, как
            // игровое (см. sage/input/Context.h).
            for (sage::input::Context* ctx : m_input->ContextsByPriority()) {
                if (!ctx->Enabled()) continue;
                for (const std::string& name : ctx->ActionNames()) {
                    const sage::input::Action* action = ctx->Find(name);
                    if (!action) continue;
                    if (wantPressed && action->WasPressed()) {
                        sol::table t = m_lua.create_table();
                        t["action"] = name;
                        DispatchEvent("action.pressed", t);
                    }
                    if (wantReleased && action->WasReleased()) {
                        sol::table t = m_lua.create_table();
                        t["action"] = name;
                        DispatchEvent("action.released", t);
                    }
                }
            }
        }
    }
}

// --- Рендер-текстуры: sage.rt.* -----------------------------------------------
//
// Съёмка сцены в именованную картинку. Сама съёмка идёт проходом рендера
// (render/ScenePasses.h, RenderTextureViews); отсюда её только заказывают.
void ScriptEngine::RegisterRenderTextureApi() {
    // Навесить съёмку на сущность: точка съёмки — её позиция.
    Bind("rt", "Attach", "AddRenderTexture", [](GameObject& obj, sol::table t) {
        if (!obj.Valid()) return;
        auto& rt = obj.Registry()->get_or_emplace<RenderTextureComponent>(obj.Entity());
        rt.Target = t.get_or("name", rt.Target);
        rt.Width = t.get_or("width", rt.Width);
        rt.Height = t.get_or("height", rt.Height);
        rt.Ortho = t.get_or("ortho", rt.Ortho);
        rt.OrthoSize = t.get_or("size", rt.OrthoSize);
        rt.Fov = t.get_or("fov", rt.Fov);
        rt.Near = t.get_or("near", rt.Near);
        rt.Far = t.get_or("far", rt.Far);
        rt.Continuous = t.get_or("continuous", rt.Continuous);
        rt.StudioLight = t.get_or("studio", rt.StudioLight);
        rt.LightIntensity = t.get_or("lightIntensity", rt.LightIntensity);
        rt.Ambient = t.get_or("ambient", rt.Ambient);
        if (sol::optional<glm::vec3> ld = t["lightDir"]) rt.LightDir = *ld;
        if (sol::optional<glm::vec3> look = t["look"]) rt.LookAt = *look;
        if (sol::optional<glm::vec4> clear = t["clear"]) rt.ClearColor = *clear;
        rt.Dirty = true;

        // Картинку заводим СРАЗУ, не дожидаясь первого прохода рендера.
        // Иначе интерфейс, собранный в OnStart (а он всегда собирается там),
        // спрашивает «готова ли иконка», получает «нет» и навсегда остаётся с
        // плоским значком: сам себя он не перестраивает.
        if (!rt.Target.empty())
            sage::render::RenderTextureRegistry::Instance().GetOrCreate(rt.Target, rt.Width,
                                                                        rt.Height);
    });

    // Пересобрать картинку. Разовая съёмка иначе застыла бы навсегда — а
    // иконка обязана обновиться, когда предмет перекрасили.
    Bind("rt", "Refresh", "RefreshRenderTexture", [](GameObject& obj) {
        if (!obj.Valid()) return;
        if (auto* rt = obj.Registry()->try_get<RenderTextureComponent>(obj.Entity()))
            rt->Dirty = true;
    });

    // Готова ли картинка с таким именем. По ней интерфейс решает, показывать
    // объёмную иконку или обойтись плоским значком.
    Bind("rt", "Ready", "RenderTextureReady", [](const std::string& name) -> bool {
        return sage::render::RenderTextureRegistry::Instance().Find(name) != nullptr;
    });
}

// --- Привязка сцены ----------------------------------------------------------
//
// Шина принадлежит сцене, а не движку: подписки уровня обязаны исчезнуть
// вместе с ним. Подписки, заведённые на прежней сцене, снимаются — иначе
// события следующего уровня доходили бы до обработчиков предыдущего.
void ScriptEngine::BindScene(Scene& scene) {
    if (m_scene && m_scene != &scene) m_scene->Events.DisconnectGroup(m_events->Group());
    m_scene = &scene;
}

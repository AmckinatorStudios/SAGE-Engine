#include "sage/scripting/ScriptingSystem.h"

#include "sage/core/Log.h"
#include "sage/physics/PhysicsScene.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::scripting {

ScriptingSystem::ScriptingSystem() = default;

ScriptingSystem::~ScriptingSystem() { Shutdown(); }

void ScriptingSystem::Bind(ScriptServices services) {
    services.Clock = &m_clock;
    services.Debug = &m_debug;
    services.System = this;
    m_runtime.Bind(services);
}

int ScriptingSystem::AttachScene(Scene& scene) {
    int attached = 0;
    auto view = scene.Registry().view<ScriptComponent>();
    for (auto e : view) {
        const ScriptComponent& sc = view.get<ScriptComponent>(e);
        if (sc.Path.empty()) continue;
        if (m_runtime.Attach(GameObject(&scene.Registry(), e), sc.Path, sc.Fields)) ++attached;
    }
    InstallLinks(scene);
    return attached;
}

int ScriptingSystem::InstallLinks(Scene& scene) {
    int n = 0;
    std::vector<entt::entity> owners;
    for (auto e : scene.Registry().view<sage::signals::SignalLinksComponent>()) owners.push_back(e);
    for (entt::entity e : owners) n += InstallLinksOf(scene, e);
    return n;
}

int ScriptingSystem::InstallLinks(Scene& scene, entt::entity only) {
    return InstallLinksOf(scene, only);
}

int ScriptingSystem::InstallLinksOf(Scene& scene, entt::entity e) {
    entt::registry& reg = scene.Registry();
    if (!reg.valid(e) || !reg.all_of<IdComponent>(e)) return 0;
    const sage::signals::SignalLinksComponent* sl = reg.try_get<sage::signals::SignalLinksComponent>(e);
    if (!sl) return 0;
    if (m_linkScene && m_linkScene != &scene) m_linkScene->Events.DisconnectGroup(m_linkGroup);
    m_linkScene = &scene;

    const int ownerId = reg.get<IdComponent>(e).Id;
    const std::string ownerName = GameObject(&reg, e).Name();
    // Повторная установка (объект пересоздан, Attach после Instantiate) не
    // должна удваивать вызовы: сначала снимаем прежние связи этого объекта.
    scene.Events.DisconnectOwner(m_linkGroup, ownerId);

    int installed = 0;
    for (const sage::signals::Link& link : sl->Links) {
        if (!link.Enabled) continue;
        const std::string title = sage::signals::Title(link.Signal);
        if (!sage::signals::IsValidName(link.Signal)) {
            LOG_ERROR("Signals") << "Связь объекта '" << ownerName << "': недопустимое имя события '"
                                 << link.Signal << "' — связь пропущена";
            continue;
        }
        std::string targetName;
        if (link.Broadcast.empty()) {
            if (!link.Target.Valid()) {
                LOG_WARN("Signals") << "Связь «" << title << "» объекта '" << ownerName
                                    << "': не выбран объект-получатель — связь пропущена";
                continue;
            }
            if (link.Method.empty()) {
                LOG_WARN("Signals") << "Связь «" << title << "» объекта '" << ownerName
                                    << "': не выбран метод — связь пропущена";
                continue;
            }
            GameObject target = scene.Get(link.Target.Id);
            if (!target.Valid()) {
                LOG_ERROR("Signals") << "Связь «" << title << "» объекта '" << ownerName
                                     << "': объект-получатель #" << link.Target.Id
                                     << " не существует — " << link.Method << "() звать не у кого";
                continue;
            }
            targetName = target.Name();
            // Проверка СРАЗУ, при запуске, а не при первом нажатии: ошибку в
            // имени метода лучше увидеть в консоли до того, как пойдёшь
            // проверять кнопку.
            if (const LiveScript* s = m_runtime.Find(target.Entity())) {
                if (!s->Backend->HasMethod(s->Instance, link.Method))
                    LOG_ERROR("Signals") << "Связь «" << title << "» объекта '" << ownerName
                                         << "': в скрипте " << s->Path << " объекта '" << targetName
                                         << "' нет метода " << link.Method;
            } else {
                LOG_ERROR("Signals") << "Связь «" << title << "» объекта '" << ownerName
                                     << "': у объекта '" << targetName << "' нет скрипта — "
                                     << link.Method << "() звать не у кого";
            }
        }

        std::weak_ptr<int> alive = m_alive;
        Scene* scenePtr = &scene;
        sage::events::ConnectOptions opt;
        opt.Group = m_linkGroup;
        opt.Owner = ownerId;
        const int id = scene.Events.Connect(
            ownerId, link.Signal,
            [this, alive, scenePtr, link, ownerName, targetName](const sage::events::Event& ev) {
                if (alive.expired()) return;
                CallLink(*scenePtr, link, ownerName, targetName, ev);
            },
            opt);
        if (id) ++installed;
    }
    return installed;
}

void ScriptingSystem::CallLink(Scene& scene, const sage::signals::Link& link,
                               const std::string& ownerName, const std::string& targetName,
                               const sage::events::Event& event) {
    const std::string title = sage::signals::Title(link.Signal);
    // Переходник старого формата: связь слала глобальное событие по имени.
    if (!link.Broadcast.empty()) {
        sage::events::Event out;
        out.Name = link.Broadcast;
        out.Arg = event.Arg;
        out.Payload = event.Payload;
        out.Sender = event.Sender;
        scene.Events.Emit(out);
        return;
    }
    GameObject target = scene.Get(link.Target.Id);
    if (!target.Valid()) {
        LOG_ERROR("Signals") << "Связь «" << title << "» объекта '" << ownerName
                             << "': объект-получатель '" << targetName << "' удалён — "
                             << link.Method << "() не вызван";
        return;
    }
    const LiveScript* s = m_runtime.Find(target.Entity());
    if (!s) {
        LOG_ERROR("Signals") << "Связь «" << title << "» объекта '" << ownerName << "': у объекта '"
                             << target.Name() << "' нет скрипта — " << link.Method << "() не вызван";
        return;
    }
    if (!s->Backend->HasMethod(s->Instance, link.Method)) {
        LOG_ERROR("Signals") << "Связь «" << title << "» объекта '" << ownerName << "': в скрипте "
                             << s->Path << " объекта '" << target.Name() << "' нет метода "
                             << link.Method;
        return;
    }
    // Ошибка внутри метода — с файлом и строкой, через общий отчёт рантайма.
    m_runtime.InvokeEvent(target.Entity(), link.Method, event);
}

bool ScriptingSystem::Attach(GameObject object) {
    if (!object.Valid()) return false;
    const ScriptComponent* sc = object.Registry()->try_get<ScriptComponent>(object.Entity());
    const bool attached = sc && !sc->Path.empty() && m_runtime.Attach(object, sc->Path, sc->Fields);
    // Связи объекта, появившегося посреди игры (префаб), ставятся так же, как
    // у объектов, бывших в сцене с начала.
    if (Scene* scene = m_runtime.Services().ScenePtr) InstallLinks(*scene, object.Entity());
    return attached;
}

void ScriptingSystem::Update(float dt) {
    const float scaled = dt * m_clock.Scale;
    m_clock.Delta = scaled;
    m_clock.Time += scaled;
    m_clock.Unscaled += dt;
    // Отжившие отладочные линии снимаются ДО Update, а не после: заказанное в
    // ЭТОМ кадре обязано дожить до отрисовки, которая случится позже него.
    // Время берётся НЕмасштабированное — отладочная метка, живущая «две
    // секунды», не должна растягиваться замедлением игры.
    m_debug.Tick(dt);
    m_runtime.Dispatch(Hook::Update, scaled);
    m_runtime.Tick(scaled);
}

void ScriptingSystem::FixedUpdate(float dt) {
    const float step = m_clock.FixedDelta;
    if (step <= 0.0f) return;
    m_fixedAccum += dt * m_clock.Scale;
    int steps = 0;
    while (m_fixedAccum >= step && steps < kMaxFixedSteps) {
        m_fixedAccum -= step;
        m_runtime.Dispatch(Hook::FixedUpdate, step);
        ++steps;
    }
    // Остаток сверх допустимого числа шагов ОТБРАСЫВАЕТСЯ, а не переносится:
    // перенос превращает одну просадку в вечную погоню за временем.
    if (steps == kMaxFixedSteps && m_fixedAccum > step) m_fixedAccum = 0.0f;
}

void ScriptingSystem::LateUpdate(float dt) {
    m_runtime.Dispatch(Hook::LateUpdate, dt * m_clock.Scale);
}

void ScriptingSystem::DispatchPhysicsEvents(PhysicsScene& physics, Scene& scene) {
    entt::registry* reg = &scene.Registry();
    // Те же удары и зоны — СИГНАЛАМИ объекта (collision, trigger_entered…):
    // на них подписываются из любого скрипта (`crate.collision:connect`) и
    // связью в инспекторе, а не только хуком скрипта самого тела. data —
    // другой объект (nil, если его уже удалили).
    auto emitContact = [&](entt::entity self, entt::entity other, bool sensor, bool begin) {
        if (!reg->valid(self) || !reg->all_of<IdComponent>(self)) return;
        const char* signal = sensor ? (begin ? sage::signals::kTriggerEntered : sage::signals::kTriggerExited)
                                    : (begin ? sage::signals::kCollision : sage::signals::kCollisionEnded);
        const int selfId = reg->get<IdComponent>(self).Id;
        if (scene.Events.Count(selfId, signal) == 0) return;   // никто не слушает — не собираем
        sage::vars::EntityRef ref;
        if (other != entt::null && reg->valid(other) && reg->all_of<IdComponent>(other))
            ref.Id = reg->get<IdComponent>(other).Id;
        scene.Events.EmitSignal(selfId, signal, sage::vars::Value(ref));
    };
    for (const PhysicsScene::EntityContact& c : physics.Contacts()) {
        if (c.A == entt::null || c.B == entt::null) continue;
        // Зона (сенсор) и удар — РАЗНЫЕ хуки: «вошёл в триггер» и «ударился»
        // это разные события игры, и путать их значит заставлять каждый скрипт
        // выяснять, что с ним произошло, по косвенным признакам.
        const Hook hook = c.Sensor ? (c.Begin ? Hook::OnTriggerEnter : Hook::OnTriggerExit)
                                   : (c.Begin ? Hook::OnCollisionEnter : Hook::OnCollisionExit);
        if (reg->valid(c.A) && reg->valid(c.B)) {
            m_runtime.DispatchTo(c.A, hook, GameObject(reg, c.B));
            m_runtime.DispatchTo(c.B, hook, GameObject(reg, c.A));
            emitContact(c.A, c.B, c.Sensor, c.Begin);
            emitContact(c.B, c.A, c.Sensor, c.Begin);
        } else if (c.Sensor && !c.Begin) {
            // Гостя удалили, пока он стоял в зоне. Выход всё равно сообщается —
            // оставшейся стороне, с other = nil: скрипт, считающий «сколько
            // внутри», иначе навсегда остался бы на единицу впереди.
            if (reg->valid(c.A)) {
                m_runtime.DispatchTo(c.A, hook, GameObject(reg, c.B));
                emitContact(c.A, c.B, c.Sensor, c.Begin);
            }
            if (reg->valid(c.B)) {
                m_runtime.DispatchTo(c.B, hook, GameObject(reg, c.A));
                emitContact(c.B, c.A, c.Sensor, c.Begin);
            }
        }
    }
    // Пребывание в зоне — тем, кто в ней уже не первый шаг. Обеим сторонам, как
    // и вход: и зоне («кто стоит на плите»), и гостю («я в воде»).
    for (const PhysicsScene::TriggerOverlap& o : physics.TriggerOverlaps()) {
        if (o.Entered || !reg->valid(o.Trigger) || !reg->valid(o.Other)) continue;
        m_runtime.DispatchTo(o.Trigger, Hook::OnTriggerStay, GameObject(reg, o.Other));
        m_runtime.DispatchTo(o.Other, Hook::OnTriggerStay, GameObject(reg, o.Trigger));
    }
}

void ScriptingSystem::DispatchAnimationEvent(GameObject object, const std::string& name) {
    if (!object.Valid()) return;
    m_runtime.DispatchNamed(object.Entity(), Hook::OnAnimationEvent, name);
}

void ScriptingSystem::RequestScene(const std::string& name) {
    // Первый запрос кадра выигрывает: два скрипта, попросивших разные уровни в
    // одном кадре, — ошибка игры, и молча выполнить ПОСЛЕДНИЙ значило бы, что
    // исход зависит от порядка обхода сущностей.
    if (m_sceneRequested) return;
    m_pendingScene = name;
    m_sceneRequested = true;
}

bool ScriptingSystem::TakeSceneRequest(std::string& name) {
    if (!m_sceneRequested) return false;
    m_sceneRequested = false;
    name = m_pendingScene;
    m_pendingScene.clear();
    return true;
}

void ScriptingSystem::Shutdown() {
    if (m_linkScene) m_linkScene->Events.DisconnectGroup(m_linkGroup);
    m_linkScene = nullptr;
    m_runtime.DetachAll();
    m_debug.Clear();
    // Незабранный запрос не имеет права пережить остановку: иначе следующий
    // запуск начался бы с чужой сцены, попрошенной в прошлой жизни.
    m_sceneRequested = false;
    m_pendingScene.clear();
}

} // namespace sage::scripting

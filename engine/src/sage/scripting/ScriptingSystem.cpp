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
    return attached;
}

bool ScriptingSystem::Attach(GameObject object) {
    if (!object.Valid()) return false;
    const ScriptComponent* sc = object.Registry()->try_get<ScriptComponent>(object.Entity());
    if (!sc || sc->Path.empty()) return false;
    return m_runtime.Attach(object, sc->Path, sc->Fields);
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
        }
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
    m_runtime.DetachAll();
    m_debug.Clear();
    // Незабранный запрос не имеет права пережить остановку: иначе следующий
    // запуск начался бы с чужой сцены, попрошенной в прошлой жизни.
    m_sceneRequested = false;
    m_pendingScene.clear();
}

} // namespace sage::scripting

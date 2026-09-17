#pragma once
#include <memory>

// ---------------------------------------------------------------------------
// EngineContext — ЯВНЫЙ владелец подсистем, живущих «одна на процесс».
//
// Зачем он есть. Кэш ресурсов, база ассетов, пул задач, каталог пост-эффектов,
// набор render-текстур, реестр импортёров и настройки были семью независимыми
// функциями-статиками (`static X inst; return inst;`). У такой конструкции три
// беды, и все три уже стоили движку крови:
//
//   1. ВРЕМЯ ЖИЗНИ НИКОМУ НЕ ПРИНАДЛЕЖИТ. Статик разрушается обработчиком
//      выхода процесса — то есть ПОСЛЕ окна и графического устройства. Кэш
//      ресурсов освобождал буферы видеокарты, когда контекста уже нет, и
//      движок падал с кодом 139 в момент, когда в логе всё написано и причина
//      не видна (см. tests/exit_probe.cpp и docs/history.md). Лечилось это
//      сначала ручным `Clear()` у каждого потребителя, потом сознательной
//      утечкой (`static X* r = new X()` в RenderTextureRegistry) — оба лечения
//      прячут болезнь, а не убирают её.
//   2. ДВУХ ЭКЗЕМПЛЯРОВ НЕ БЫВАЕТ. Тест не может поднять чистый кэш ресурсов,
//      не задев соседний тест в том же процессе: состояние течёт между
//      проверками, и порядок запуска начинает влиять на результат.
//   3. ЗАВИСИМОСТЬ НЕ ВИДНА В ПОДПИСИ. `ResourceManager::Instance()` посреди
//      функции не виден ни в заголовке, ни в конструкторе — узнать, что класс
//      требует живого кэша, можно только прочитав его целиком.
//
// Что делает контекст. Он ВЛАДЕЕТ всеми семью подсистемами и разрушает их в
// порядке, обратном созданию, ПОКА ЖИВО графическое устройство: порядок
// разрушения перестаёт быть вопросом удачи и становится строкой кода.
// Application создаёт контекст первым и убивает последним.
//
// Как им пользоваться. Классу, которому нужна подсистема, её ПЕРЕДАЮТ —
// ссылкой в конструкторе или параметром. `EngineContext::Current()` оставлен
// для мест, куда ссылку пока не протащили, и для совместимости старых
// `X::Instance()`; это не рекомендуемый способ, а переходный мост.
//
// В тестах — `ScopedEngineContext`: поднимает свой контекст на время проверки
// и возвращает предыдущий обратно, так что проверки перестают делить состояние.
// ---------------------------------------------------------------------------

class ResourceManager;

namespace sage {

class JobSystem;
class AssetDatabase;
struct EngineConfig;

namespace rhi { class GraphicsDevice; }
namespace render { class PostEffectCatalog; class RenderTextureRegistry; }
namespace assets { class ImporterRegistry; }

class EngineContext {
public:
    EngineContext();
    ~EngineContext();

    EngineContext(const EngineContext&) = delete;
    EngineContext& operator=(const EngineContext&) = delete;

    // --- Подсистемы ---------------------------------------------------------
    ResourceManager& Resources() const { return *m_resources; }
    AssetDatabase& Assets() const { return *m_assets; }
    JobSystem& Jobs() const { return *m_jobs; }
    EngineConfig& Config() const { return *m_config; }
    render::PostEffectCatalog& PostEffects() const { return *m_postEffects; }
    render::RenderTextureRegistry& RenderTextures() const { return *m_renderTextures; }
    assets::ImporterRegistry& Importers() const { return *m_importers; }

    // Графическое устройство контексту НЕ принадлежит: им владеет Application
    // (оно создаётся вместе с окном и умирает вместе с ним). Контекст лишь
    // знает текущее, чтобы подсистемы спрашивали его у контекста, а не у
    // собственного статика.
    rhi::GraphicsDevice* Device() const { return m_device; }
    void SetDevice(rhi::GraphicsDevice* device) { m_device = device; }

    // Освобождает ресурсы видеокарты, пока устройство ещё живо. Зовётся
    // Application'ом перед сносом устройства; идемпотентен.
    void ReleaseGpuResources();

    // --- Текущий контекст процесса -----------------------------------------
    //
    // Бросает, если контекста нет: «подсистема запрошена вне Application» —
    // это ошибка сборки программы, а не ситуация, которую стоит молча терпеть.
    static EngineContext& Current();
    static EngineContext* CurrentOrNull() { return s_current; }
    // Возвращает предыдущий — вызывающий обязан вернуть его на место.
    static EngineContext* SetCurrent(EngineContext* context);

private:
    std::unique_ptr<ResourceManager> m_resources;
    std::unique_ptr<AssetDatabase> m_assets;
    std::unique_ptr<JobSystem> m_jobs;
    std::unique_ptr<EngineConfig> m_config;
    std::unique_ptr<render::PostEffectCatalog> m_postEffects;
    std::unique_ptr<render::RenderTextureRegistry> m_renderTextures;
    std::unique_ptr<assets::ImporterRegistry> m_importers;

    rhi::GraphicsDevice* m_device = nullptr;

    static EngineContext* s_current;
};

// Контекст на время области видимости — для тестов и утилит, у которых нет
// Application. Предыдущий контекст восстанавливается, даже если проверка упала
// с исключением.
class ScopedEngineContext {
public:
    ScopedEngineContext() : m_previous(EngineContext::SetCurrent(&m_context)) {}
    // Контекст разрушается ДО восстановления предыдущего: он же освобождает
    // свои GPU-объекты, и делать это надо, пока текущим считается он.
    ~ScopedEngineContext() { EngineContext::SetCurrent(m_previous); }

    ScopedEngineContext(const ScopedEngineContext&) = delete;
    ScopedEngineContext& operator=(const ScopedEngineContext&) = delete;

    EngineContext& operator*() { return m_context; }
    EngineContext* operator->() { return &m_context; }
    EngineContext& Get() { return m_context; }

private:
    EngineContext m_context;
    EngineContext* m_previous;
};

} // namespace sage

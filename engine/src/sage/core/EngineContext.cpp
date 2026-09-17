#include "sage/core/EngineContext.h"

#include "sage/core/Config.h"
#include "sage/core/JobSystem.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/import/Importer.h"
#include "sage/render/PostEffect.h"
#include "sage/render/RenderTexture.h"
#include "sage/render/ResourceManager.h"

#include <stdexcept>

namespace sage {

EngineContext* EngineContext::s_current = nullptr;

// Порядок создания — это и есть объявленный порядок зависимостей: настройки и
// пул задач ни от кого не зависят, кэш ресурсов и наборы GPU-объектов идут
// последними, потому что разрушать их надо ПЕРВЫМИ (см. деструктор).
EngineContext::EngineContext()
    : m_resources(std::make_unique<ResourceManager>()),
      m_assets(std::make_unique<AssetDatabase>()),
      m_jobs(std::make_unique<JobSystem>()),
      m_config(std::make_unique<EngineConfig>()),
      m_postEffects(std::make_unique<render::PostEffectCatalog>()),
      m_renderTextures(std::make_unique<render::RenderTextureRegistry>()),
      m_importers(std::make_unique<assets::ImporterRegistry>()) {}

void EngineContext::ReleaseGpuResources() {
    // Всё, что держит буферы видеокарты, освобождается ЗДЕСЬ — по явному
    // вызову Application'а, пока устройство ещё живо. Раньше это происходило
    // само собой в обработчике выхода процесса, то есть уже без устройства, и
    // движок падал на выходе. Оба вызова идемпотентны.
    if (m_renderTextures) m_renderTextures->Clear();
    if (m_resources) m_resources->Clear();
}

EngineContext::~EngineContext() {
    ReleaseGpuResources();
    // Воркеры останавливаются до сноса подсистем: их задачи могут держать
    // ссылки на кэш ресурсов.
    if (m_jobs) m_jobs->Shutdown();

    // Явный обратный порядок. Полагаться на порядок полей можно, но тогда
    // перестановка двух строк в заголовке меняет порядок разрушения молча —
    // ровно тот класс поломок, ради которого этот класс и появился.
    m_importers.reset();
    m_renderTextures.reset();
    m_postEffects.reset();
    m_config.reset();
    m_jobs.reset();
    m_assets.reset();
    m_resources.reset();

    if (s_current == this) s_current = nullptr;
}

EngineContext& EngineContext::Current() {
    if (!s_current)
        throw std::runtime_error(
            "EngineContext::Current: подсистема запрошена вне контекста движка "
            "(нет активного Application или ScopedEngineContext)");
    return *s_current;
}

EngineContext* EngineContext::SetCurrent(EngineContext* context) {
    EngineContext* previous = s_current;
    s_current = context;
    return previous;
}

} // namespace sage

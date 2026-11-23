#include "sage/core/EngineContext.h"

#include "sage/core/Config.h"
#include "sage/core/JobSystem.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/import/Importer.h"
#include "sage/render/PostEffect.h"
#include "sage/render/RenderTexture.h"
#include "sage/render/ResourceManager.h"

namespace sage {

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
    // На время сноса текущим делается ЭТОТ контекст: подсистемы, которые при
    // разрушении спрашивают EngineContext::Current() (кэш ресурсов, воркеры),
    // должны попасть в себя, а не в соседний контекст. Чужой текущий
    // возвращается на место в конце; свой — обнуляется.
    EngineContext* const previous = SetCurrent(this);

    // 1. Воркеры — ПЕРВЫМИ: их задачи держат ссылки на кэш ресурсов и прочие
    //    подсистемы, и чистить кэш под работающей задачей нельзя. Выключаем
    //    пул, чтобы ParallelFor из чьего-нибудь деструктора не поднял его
    //    заново ленивой инициализацией.
    if (m_jobs) {
        m_jobs->Shutdown();
        m_jobs->SetEnabled(false);
    }
    // 2. GPU-объекты (идемпотентно: Application обычно уже освободил их).
    ReleaseGpuResources();

    // 3. Явный обратный порядок. Полагаться на порядок полей можно, но тогда
    //    перестановка двух строк в заголовке меняет порядок разрушения молча —
    //    ровно тот класс поломок, ради которого этот класс и появился.
    m_importers.reset();
    m_renderTextures.reset();
    m_postEffects.reset();
    m_config.reset();
    m_jobs.reset();
    m_assets.reset();
    m_resources.reset();
    m_device = nullptr;

    s_current = (previous == this) ? nullptr : previous;
}

} // namespace sage

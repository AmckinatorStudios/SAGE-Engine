#include "sage/ui/InterfaceSource.h"

#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/Interface.h"
#include "sage/ui/UI.h"

namespace sage::ui {
namespace {

struct CachedInterface {
    Interface Data;
    // Номер прочтения — ОБЩИЙ на все файлы и не сбрасывается вместе с кэшем:
    // иначе после ClearInterfaceSourceCache новое прочтение получило бы тот же
    // номер 1, что и старое, и копия в сцене сочла бы себя свежей.
    int Version = 0;
    std::filesystem::file_time_type Stamp{};
    std::chrono::steady_clock::time_point Checked{};
    bool Loaded = false;
    bool Reported = false;    // об ошибке уже сказали — не повторять каждый кадр
};

std::unordered_map<std::string, CachedInterface>& Cache() {
    static std::unordered_map<std::string, CachedInterface> cache;
    return cache;
}

// Прочитать, если файл новее прочитанного. Диск — не чаще двух раз в секунду:
// синхронизация зовётся каждый кадр, а правят файл руками.
CachedInterface& Refresh(const std::string& path) {
    CachedInterface& c = Cache()[path];
    const auto now = std::chrono::steady_clock::now();
    if (c.Loaded && now - c.Checked < std::chrono::milliseconds(500)) return c;
    c.Checked = now;
    const std::string real = sage::AssetDatabase::Instance().LocatePath(path);
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(sage::PathFromUtf8(real), ec);
    // Файла на диске нет: в собранной игре он лежит в пакете, и время записи
    // у него не спросить — читаем один раз.
    if (ec && c.Loaded) return c;
    if (!ec && c.Loaded && stamp == c.Stamp) return c;
    Interface data;
    std::string err;
    if (Interface::LoadFile(ec ? path : real, data, err)) {
        c.Data = std::move(data);
        c.Stamp = stamp;
        c.Loaded = true;
        c.Reported = false;
        static int s_reads = 0;
        c.Version = ++s_reads;
    } else if (!c.Reported) {
        // Ошибка одного интерфейса не роняет остальные: этот просто остаётся
        // пустым (или прежним), а в журнале — какой файл и почему.
        LOG_WARN("UI") << "Интерфейс не прочитан (" << path << "): " << err;
        c.Reported = true;
    }
    return c;
}

void Tag(Scene& scene, entt::entity e) {
    entt::registry& reg = scene.Registry();
    reg.emplace_or_replace<InterfaceInstanceTag>(e);
    for (entt::entity c : scene.ChildrenOf(e)) Tag(scene, c);
}

void DestroyTree(Scene& scene, entt::entity e) {
    const IdComponent* id = scene.Registry().try_get<IdComponent>(e);
    if (id) scene.RemoveObject(id->Id);
}

} // namespace

void SyncInterfaceSources(Scene& scene) {
    entt::registry& reg = scene.Registry();
    // Список — заранее: развёртывание заводит сущности, и обходить вид,
    // пока реестр растёт, нельзя.
    std::vector<entt::entity> interfaces;
    for (entt::entity e : reg.view<InterfaceComponent>()) interfaces.push_back(e);

    for (entt::entity iface : interfaces) {
        if (!reg.valid(iface)) continue;
        InterfaceComponent& ic = reg.get<InterfaceComponent>(iface);
        const CachedInterface* cached = ic.Source.empty() ? nullptr : &Refresh(ic.Source);

        const bool current = cached && ic.Instance != entt::null && reg.valid(ic.Instance) &&
                             reg.all_of<InterfaceInstanceTag>(ic.Instance) &&
                             scene.ParentOf(ic.Instance) == iface &&
                             ic.InstanceSource == ic.Source &&
                             ic.InstanceVersion == cached->Version;

        // Лишние копии: объект скопировали (дубликат, заготовка) вместе с
        // развёрнутым — у копии они чужие, и она развернёт свои.
        std::vector<entt::entity> stale;
        for (entt::entity c : scene.ChildrenOf(iface))
            if (reg.all_of<InterfaceInstanceTag>(c) && !(current && c == ic.Instance))
                stale.push_back(c);
        for (entt::entity c : stale) DestroyTree(scene, c);
        if (current) continue;

        // ic — ссылка в реестр, а удаление и создание сущностей могли его
        // перестроить: берём заново.
        InterfaceComponent& fresh = reg.get<InterfaceComponent>(iface);
        fresh.Instance = entt::null;
        fresh.InstanceSource = fresh.Source;
        fresh.InstanceVersion = cached ? cached->Version : -1;
        if (!cached || !cached->Loaded) continue;

        const Interface data = cached->Data;   // копия: развёртывание не должно держать ссылку в кэш
        fresh.Canvas = data.Canvas;
        const entt::entity root = BuildMainContainer(scene, data, iface);
        if (root == entt::null) continue;
        Tag(scene, root);
        reg.get<InterfaceComponent>(iface).Instance = root;
    }
}

bool IsInstancePart(const Scene& scene, entt::entity e) {
    const entt::registry& reg = scene.Registry();
    for (entt::entity a = e; a != entt::null && reg.valid(a); a = scene.ParentOf(a))
        if (reg.all_of<InterfaceInstanceTag>(a)) return true;
    return false;
}

void ClearInterfaceSourceCache() { Cache().clear(); }

} // namespace sage::ui

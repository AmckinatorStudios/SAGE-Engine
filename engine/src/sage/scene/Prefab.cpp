#include "sage/scene/Prefab.h"

#include <memory>
#include <unordered_map>
#include <vector>

#include "sage/core/Log.h"
#include "sage/scene/ComponentRegistry.h"
#include "sage/scene/Components.h"
#include "sage/render/LodGroup.h"
#include "sage/scene/Signals.h"
#include "sage/ui/components/InterfaceComponent.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPart.h"
#include "sage/vars/Refs.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

namespace sage::scene {
namespace {

template <typename T>
void CopyIfPresent(GameObject& src, GameObject& dst) {
    if (const T* c = src.Registry()->try_get<T>(src.Entity())) {
        dst.Registry()->emplace_or_replace<T>(dst.Entity(), *c);
    }
}

// Разобранные префабы. Ключ — путь как его дали: тот же путь придёт и в
// следующий раз, а нормализовать его тут значило бы завести вторую политику
// путей рядом с той, что уже есть в базе ассетов.
std::unordered_map<std::string, std::shared_ptr<Scene>>& Cache() {
    static std::unordered_map<std::string, std::shared_ptr<Scene>> cache;
    return cache;
}

std::shared_ptr<Scene> LoadCached(const std::string& path) {
    auto& cache = Cache();
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    std::shared_ptr<Scene> loaded;
    try {
        loaded = std::shared_ptr<Scene>(SceneSerializer::Load(path).release());
    } catch (const std::exception& e) {
        LOG_ERROR("Prefab") << "Префаб не загрузился (" << path << "): " << e.what();
        // Неудачу тоже кладём в кэш — иначе игра, спавнящая блоки каждый кадр,
        // каждый кадр же ломилась бы в отсутствующий файл и заливала лог.
        cache[path] = nullptr;
        return nullptr;
    }
    cache[path] = loaded;
    return loaded;
}

// Корни префаба — сущности без живого родителя.
std::vector<entt::entity> RootsOf(Scene& scene) {
    std::vector<entt::entity> roots;
    auto& reg = scene.Registry();
    for (auto e : reg.view<IdComponent>()) {
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
        if (!h || h->Parent == entt::null || !reg.valid(h->Parent)) roots.push_back(e);
    }
    return roots;
}

} // namespace

void CopyAllComponents(GameObject& src, GameObject& dst) {
    dst.GetTransform() = src.GetTransform();
    // МЕШ КОПИРУЕТСЯ, ЕСЛИ ОН ЕСТЬ. Копия пустого объекта обязана остаться
    // пустой: иначе дублирование узла иерархии возвращало бы ему MeshRenderer,
    // и «пустой» объект переставал быть пустым от одного Ctrl+D.
    if (const MeshRendererComponent* smr = src.Registry()->try_get<MeshRendererComponent>(src.Entity())) {
        dst.Registry()->emplace_or_replace<MeshRendererComponent>(dst.Entity(), *smr);
    } else {
        dst.Registry()->remove<MeshRendererComponent>(dst.Entity());
    }
    // Копия папки — тоже папка: иначе дубликат группы (Ctrl+D) превращался бы
    // в обычный пустой объект и начинал двигать содержимое.
    // Метка-пустышка: у неё нет полей, и try_get<> для такого типа возвращает
    // void — общий CopyIfPresent на ней не собирается. Копируем фактом наличия.
    if (src.Registry()->all_of<HiddenComponent>(src.Entity())) {
        dst.Registry()->emplace_or_replace<HiddenComponent>(dst.Entity());
    } else {
        dst.Registry()->remove<HiddenComponent>(dst.Entity());
    }
    // Метки без полей — так же, фактом наличия.
    auto copyMark = [&](auto tag) {
        using M = decltype(tag);
        if (src.Registry()->all_of<M>(src.Entity())) dst.Registry()->emplace_or_replace<M>(dst.Entity());
        else dst.Registry()->remove<M>(dst.Entity());
    };
    copyMark(KeepOnLoadComponent{});
    CopyIfPresent<FolderComponent>(src, dst);
    // Тег, звук, твины, наклейка, LOD, пост-обработка, сеть, съёмка в
    // текстуру. Этого списка здесь не было — и копия (Ctrl+D, префаб,
    // вставка) теряла их молча: сериализатор их писал, а копирование — нет.
    // Сторожит tests/test_scene_lossless.cpp.
    CopyIfPresent<TagComponent>(src, dst);
    CopyIfPresent<AudioSourceComponent>(src, dst);
    CopyIfPresent<sage::anim::TweenComponent>(src, dst);
    CopyIfPresent<PropertyAnimatorComponent>(src, dst);
    CopyIfPresent<DecalComponent>(src, dst);
    CopyIfPresent<sage::render::LodComponent>(src, dst);
    CopyIfPresent<sage::render::PostProcessComponent>(src, dst);
    CopyIfPresent<sage::ui::InterfaceComponent>(src, dst);
    CopyIfPresent<NetReplicatedComponent>(src, dst);
    CopyIfPresent<RenderTextureComponent>(src, dst);
    CopyIfPresent<ScriptComponent>(src, dst);
    // Компоненты игры (реестр) и компоненты, которых этот код не знает.
    CopyRegisteredComponents(*src.Registry(), src.Entity(), *dst.Registry(), dst.Entity());
    // Публичные переменные едут с объектом: префаб без своих настроек — это
    // объект, который после постановки в сцену надо настраивать заново.
    CopyIfPresent<VarsComponent>(src, dst);
    // Связи сигналов — тоже: кнопка из префаба без своих «On Click» — мёртвая.
    CopyIfPresent<sage::signals::SignalLinksComponent>(src, dst);
    CopyIfPresent<CameraComponent>(src, dst);
    CopyIfPresent<LightComponent>(src, dst);
    CopyIfPresent<RigidBodyComponent>(src, dst);
    CopyIfPresent<ColliderComponent>(src, dst);
    CopyIfPresent<JointComponent>(src, dst);
    CopyIfPresent<ParticleEmitterComponent>(src, dst);
    CopyIfPresent<AnimationComponent>(src, dst);
    CopyIfPresent<IKComponent>(src, dst);
    CopyIfPresent<ReflectionProbeComponent>(src, dst);
    // Интерфейс — ПО РЕЕСТРУ ЧАСТЕЙ, а не списком руками.
    //
    // Список здесь был, и он молча устаревал: часть, зарегистрированную игрой
    // (sage::ui::RegisterPart), он не знал по определению — такая часть
    // переживала сохранение сцены и пропадала при дублировании объекта.
    // Объяснить это можно было только чтением исходников движка.
    CopyIfPresent<sage::ui::Element>(src, dst);   // прямоугольник — не часть, а сам элемент
    for (const sage::ui::PartType& part : sage::ui::Parts()) {
        if (part.Copy) part.Copy(*src.Registry(), src.Entity(), *dst.Registry(), dst.Entity());
    }
    CopyIfPresent<GIStaticComponent>(src, dst);
    CopyIfPresent<CharacterControllerComponent>(src, dst);
    CopyIfPresent<ShaderParamsComponent>(src, dst);

    // Дальше — сброс того, что принадлежит ЭКЗЕМПЛЯРУ, а не шаблону. Скопируй
    // мы это как есть, две сущности делили бы один дескриптор тела, и удаление
    // одной уносило бы физику другой.
    if (auto* rb = dst.Registry()->try_get<RigidBodyComponent>(dst.Entity()))
        rb->RuntimeBody = sage::physics::kInvalidBody;
    if (auto* cc = dst.Registry()->try_get<CharacterControllerComponent>(dst.Entity())) {
        cc->Runtime = sage::physics::kInvalidCharacter;
        cc->Grounded = false;
    }
    if (auto* jc = dst.Registry()->try_get<JointComponent>(dst.Entity()))
        jc->RuntimeJoint = sage::physics::kInvalidJoint;
    if (auto* am = dst.Registry()->try_get<AnimationComponent>(dst.Entity())) {
        am->Model.reset();
        am->Anim = sage::anim::Animator{};
        am->Ready = false;
        // И ПУТЬ, из которого собран скелет: он тоже рантайм. Оставленный, он
        // совпал бы с путём в Mesh копии, и система решила бы, что скелет уже
        // готов, — а Model при этом пуста, то есть копия осталась бы без
        // анимации до первой смены модели.
        am->ResolvedFrom.clear();
    }
    // Индексы костей и залипшая опора — состояние экземпляра: у копии модель
    // загрузится заново, и цели должны разрешиться по именам с нуля.
    if (auto* ik = dst.Registry()->try_get<IKComponent>(dst.Entity()))
        for (IKGoal& g : ik->Goals) { g.Resolved = false; g.Locked = false; }
    // Снятая карта принадлежит ТОЧКЕ, а копия стоит в другой: делить её нельзя,
    // иначе копия отражала бы окружение оригинала.
    if (auto* rp = dst.Registry()->try_get<ReflectionProbeComponent>(dst.Entity())) {
        rp->Runtime.reset();
        rp->Dirty = true;
    }
    if (auto* pe = dst.Registry()->try_get<ParticleEmitterComponent>(dst.Entity())) {
        pe->PendingEmit = 0;
        pe->RestartRequested = pe->ClearRequested = false;
    }
    // Голос звука принадлежит оригиналу: копия с его номером глушила бы чужой
    // звук и считала бы себя уже играющей (AutoPlay не сработал бы).
    if (auto* au = dst.Registry()->try_get<AudioSourceComponent>(dst.Entity())) {
        au->Handle = 0;
        au->Playing = au->Paused = false;
        au->Request = AudioRequest::None;
    }
    // «Уже запущены при старте» — про оригинал; копия запускает свои сама.
    if (auto* tw = dst.Registry()->try_get<sage::anim::TweenComponent>(dst.Entity())) tw->Started = false;
    // Геометрия наклейки строится по месту — у копии место другое.
    if (auto* dc = dst.Registry()->try_get<DecalComponent>(dst.Entity())) {
        dc->Dirty = true;
        dc->Triangles = 0;
    }
    if (auto* pa = dst.Registry()->try_get<PropertyAnimatorComponent>(dst.Entity())) {
        pa->Clip.reset();
        pa->Ready = false;
        pa->Time = 0.0f;
    }
    if (auto* rt = dst.Registry()->try_get<RenderTextureComponent>(dst.Entity())) rt->Dirty = true;
}

namespace {

// Копирует поддерево и заодно собирает «старый id -> новый id» и список
// созданных сущностей: без них ссылки внутри копии переписать нечем.
GameObject CopySubtreeCollecting(Scene& src, entt::entity srcRoot, Scene& dst,
                                 entt::entity dstParent, std::unordered_map<int, int>& idMap,
                                 std::vector<entt::entity>& made) {
    GameObject s(&src.Registry(), srcRoot);
    GameObject d = dst.CreateObject(s.Name());
    CopyAllComponents(s, d);
    if (dstParent != entt::null) dst.SetParent(d.Entity(), dstParent);
    // Место среди братьев едет с копией: дети заготовки встают в том порядке,
    // в каком их разложили, а дубликат — сразу за оригиналом (номер у него
    // больше, место то же).
    dst.Registry().get_or_emplace<HierarchyComponent>(d.Entity()).Order = src.SiblingOrder(srcRoot);
    if (const IdComponent* srcId = src.Registry().try_get<IdComponent>(srcRoot))
        idMap[srcId->Id] = d.Id();
    made.push_back(d.Entity());
    if (const HierarchyComponent* h = src.Registry().try_get<HierarchyComponent>(srcRoot)) {
        // В порядке списка: Children у копии собирается в той же
        // последовательности, в какой его обходят системы.
        const std::vector<entt::entity> kids = src.ChildrenOf(srcRoot);
        for (entt::entity k : kids)
            if (src.Registry().valid(k))
                CopySubtreeCollecting(src, k, dst, d.Entity(), idMap, made);
    }
    return d;
}

} // namespace

// ССЫЛКИ ВНУТРИ КОПИИ ВЕДУТ В КОПИЮ.
//
// Ссылка хранит номер объекта, а у копии номера другие. Не переписав их, мы
// получаем заготовку, которая работает ровно один раз: вторая поставленная
// дверь открывается кнопкой ПЕРВОЙ, а десятая не открывается вовсе, потому что
// такого номера в сцене уже нет. Ломается это молча — в инспекторе ссылка
// выглядит заполненной, и она даже указывает на существующий объект.
//
// Ссылка НАРУЖУ поддерева остаётся как была: дублируя лампу, которая светит на
// игрока, человек ждёт вторую лампу, светящую на того же игрока, а не
// оборванную ссылку.
void RemapEntityRefs(Scene& scene, const std::vector<entt::entity>& entities,
                     const std::unordered_map<int, int>& idMap) {
    for (entt::entity e : entities) {
        if (!scene.Registry().valid(e)) continue;
        sage::scene::VisitEntityRefs(scene.Registry(), e, [&](sage::vars::EntityRef& ref) {
            auto it = idMap.find(ref.Id);
            if (it != idMap.end()) ref.Id = it->second;
        });
    }
}

GameObject CopySubtree(Scene& src, entt::entity srcRoot, Scene& dst, entt::entity dstParent) {
    std::unordered_map<int, int> idMap;
    std::vector<entt::entity> made;
    GameObject d = CopySubtreeCollecting(src, srcRoot, dst, dstParent, idMap, made);
    RemapEntityRefs(dst, made, idMap);
    return d;
}

bool SavePrefab(Scene& scene, entt::entity root, const std::string& path, std::string& err) {
    if (!scene.Registry().valid(root)) {
        err = "сущность не существует";
        return false;
    }
    Scene temp("Prefab");
    std::unordered_map<int, int> idMap;
    std::vector<entt::entity> made;
    CopySubtreeCollecting(scene, root, temp, entt::null, idMap, made);

    // ССЫЛКИ НАРУЖУ ЗАГОТОВКИ ОБРЫВАЮТСЯ, И ЭТО НЕ ПОТЕРЯ ДАННЫХ.
    //
    // Префаб — файл, который ставят в ЛЮБУЮ сцену и сколько угодно раз. Ссылка
    // на объект, оставшийся в исходной сцене, там не значит ничего: номер либо
    // не найдётся, либо — что хуже — попадёт в посторонний объект, случайно
    // получивший его. Второе не отличить от работающей связи ни в инспекторе,
    // ни глазами: заготовка «дверь» будет исправно дёргать чей-то фонарь.
    //
    // Поэтому такие ссылки обнуляются, а автор узнаёт об этом из лога — молча
    // обрубить связь значило бы поменять поведение заготовки без предупреждения.
    // Обрыв идёт ДО переписывания номеров и решается ПО КАРТЕ, а не поиском в
    // получившейся заготовке. Разница не тонкая: номера в заготовке начинаются
    // с единицы, и «внешний» номер из исходной сцены запросто совпадёт с
    // номером объекта ВНУТРИ неё — тогда проверка «а есть ли такой?» ответит
    // «есть», и ссылка на игрока молча превратится в ссылку на дверную ручку.
    int dropped = 0;
    for (entt::entity e : made) {
        if (!temp.Registry().valid(e)) continue;
        sage::scene::VisitEntityRefs(temp.Registry(), e, [&](sage::vars::EntityRef& ref) {
            if (!ref.Valid()) return;
            if (idMap.count(ref.Id) != 0) return;   // внутрь заготовки — оставляем
            ref.Id = 0;
            ++dropped;
        });
    }
    RemapEntityRefs(temp, made, idMap);
    if (dropped > 0) {
        LOG_WARN("Prefab") << "В заготовке " << path << " оборвано ссылок наружу: " << dropped
                           << " — префаб ставят в любую сцену, и объекта из исходной там нет";
    }

    try {
        SceneSerializer::Save(temp, path);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    // Файл переписан — разобранная копия в кэше устарела.
    Cache().erase(path);
    return true;
}

int InstantiatePrefab(Scene& scene, const std::string& path) {
    std::shared_ptr<Scene> prefab = LoadCached(path);
    if (!prefab) return -1;

    // ВСЕ корни префаба переносятся ОДНОЙ операцией, и карта номеров у них
    // общая: связь между двумя корнями одного файла (кнопка в одном, дверь в
    // другом) иначе указывала бы наружу — то есть в чужой объект сцены,
    // случайно получивший тот же номер.
    std::unordered_map<int, int> idMap;
    std::vector<entt::entity> made;
    int firstRootId = -1;
    for (entt::entity r : RootsOf(*prefab)) {
        GameObject copy = CopySubtreeCollecting(*prefab, r, scene, entt::null, idMap, made);
        if (firstRootId == -1) firstRootId = copy.Id();
    }
    // Ссылка, которой в карте нет, в ЗАГОТОВКЕ означать может только одно —
    // что файл сделан до того, как обрыв внешних ссылок появился при
    // сохранении. Оставить её нельзя: номер из чужой сцены попадёт в
    // посторонний объект этой, и «работающая» связь будет дёргать не то.
    for (entt::entity e : made) {
        if (!scene.Registry().valid(e)) continue;
        sage::scene::VisitEntityRefs(scene.Registry(), e, [&](sage::vars::EntityRef& ref) {
            if (ref.Valid() && idMap.count(ref.Id) == 0) ref.Id = 0;
        });
    }
    RemapEntityRefs(scene, made, idMap);
    return firstRootId;
}

int InstantiatePrefabAt(Scene& scene, const std::string& path, const glm::vec3& position) {
    const int id = InstantiatePrefab(scene, path);
    if (id < 0) return id;
    GameObject root = scene.Get(id);
    if (root.Valid()) root.GetTransform().Position = position;
    return id;
}

void ClearPrefabCache() { Cache().clear(); }

} // namespace sage::scene

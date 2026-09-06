#include "sage/ui/scene/UIScene.h"

#include <algorithm>

#include "sage/core/Log.h"
#include "sage/render/Framebuffer.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UIRenderer.h"

namespace sage::ui {

UIDocuments& UIDocuments::Instance() {
    // Живёт до конца процесса и НЕ разрушается: в рантаймах лежат текстуры и
    // шрифты, а разрушать их после закрытия графического контекста — падение.
    // Тот же приём, что у реестра рендер-текстур движка.
    static UIDocuments* instance = new UIDocuments();
    return *instance;
}

void UIDocuments::SetLocalizer(std::function<std::string(const std::string&)> fn) {
    m_localize = std::move(fn);
    for (auto& e : m_docs)
        if (e->Runtime) e->Runtime->Context().Localize = m_localize;
}

UIRuntime& UIDocuments::GetOrLoad(const std::string& path) {
    for (auto& e : m_docs)
        if (e->Path == path) return *e->Runtime;

    if (!m_resources) m_resources = std::make_unique<UIEngineResources>();
    auto entry = std::make_unique<Entry>();
    entry->Path = path;
    entry->Runtime = std::make_unique<UIRuntime>();
    m_resources->Install(entry->Runtime->Context());
    entry->Runtime->Context().Localize = m_localize;

    const UILoadReport report = UILoadDocument(entry->Runtime->Doc(), path,
                                               &entry->Runtime->Theme());
    entry->Loaded = report.Ok;
    if (!report.Ok) {
        // Документа нет или он битый — это НЕ повод падать и не повод молчать:
        // игра продолжает работать, а причина написана в логе (§134).
        LOG_WARN("UI") << "документ интерфейса не прочитан: " << path << " — "
                       << (report.Error.empty() ? "нет файла" : report.Error);
    }
    m_docs.push_back(std::move(entry));
    return *m_docs.back()->Runtime;
}

UIRuntime* UIDocuments::Find(const std::string& path) {
    for (auto& e : m_docs)
        if (e->Path == path) return e->Runtime.get();
    return nullptr;
}

bool UIDocuments::Loaded(const std::string& path) const {
    for (const auto& e : m_docs)
        if (e->Path == path) return e->Loaded;
    return false;
}

bool UIDocuments::Reload(const std::string& path) {
    for (auto& e : m_docs) {
        if (e->Path != path) continue;
        const UILoadReport r = UILoadDocument(e->Runtime->Doc(), path, &e->Runtime->Theme());
        e->Loaded = r.Ok;
        return r.Ok;
    }
    return false;
}

void UIDocuments::Remove(const std::string& path) {
    for (size_t i = 0; i < m_docs.size(); ++i) {
        if (m_docs[i]->Path == path) { m_docs.erase(m_docs.begin() + (long)i); return; }
    }
}

void UIDocuments::Clear() { m_docs.clear(); }

std::vector<std::string> UIDocuments::OpenPaths() const {
    std::vector<std::string> out;
    out.reserve(m_docs.size());
    for (const auto& e : m_docs) out.push_back(e->Path);
    return out;
}

UISceneRuntime::UISceneRuntime() = default;

std::vector<UISceneRuntime::Open> UISceneRuntime::Collect(Scene& scene) {
    struct Item { int Order; Open Doc; };
    std::vector<Item> items;
    scene.Registry().view<UIDocumentComponent>().each(
        [&](entt::entity e, const UIDocumentComponent& c) {
            if (!c.Visible || c.Path.empty()) return;
            items.push_back({c.SortOrder, Open{e, &UIDocuments::Instance().GetOrLoad(c.Path),
                                               c.Interactive}});
        });
    // Порядок между документами задаётся числом, а не порядком объектов в
    // сцене: меню паузы обязано лечь поверх худа независимо от того, кого
    // создали раньше.
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.Order < b.Order; });
    std::vector<Open> out;
    out.reserve(items.size());
    for (const Item& i : items) out.push_back(i.Doc);
    return out;
}

bool UISceneRuntime::Any(Scene& scene) const {
    auto view = scene.Registry().view<UIDocumentComponent>();
    return view.begin() != view.end();
}

void UISceneRuntime::Update(Scene& scene, glm::vec2 screenPixels, float dt) {
    m_profile = UIProfile{};
    for (const Open& doc : Collect(scene)) {
        UIRuntime* rt = doc.Runtime;
        rt->SetScreen(screenPixels);
        rt->Update(dt);
        const UIProfile& p = rt->Profile();
        m_profile.Layout.Nodes += p.Layout.Nodes;
        m_profile.Layout.Visible += p.Layout.Visible;
        m_profile.Layout.Culled += p.Layout.Culled;
        m_profile.Layout.LayoutMs += p.Layout.LayoutMs;
    }
}

UIInputReport UISceneRuntime::HandleInput(Scene& scene, const UIInputFrame& input,
                                          glm::vec2 screenPixels) {
    UIInputReport merged;
    std::vector<Open> docs = Collect(scene);
    // Сверху вниз: верхний документ имеет право забрать ввод себе, и нижние его
    // тогда не видят. Иначе клик по меню паузы проходил бы ещё и в худ.
    for (auto it = docs.rbegin(); it != docs.rend(); ++it) {
        UIRuntime* rt = it->Runtime;
        rt->SetScreen(screenPixels);
        UIInputFrame frame = input;
        // Непринимающий ввод документ всё равно считается: у него живут
        // анимации и состояния, а вот мышь до него не доходит.
        if (merged.PointerOverUI || !it->Interactive) frame.PointerInside = false;
        const UIInputReport r = rt->HandleInput(frame);
        merged.PointerOverUI = merged.PointerOverUI || r.PointerOverUI;
        merged.PointerCaptured = merged.PointerCaptured || r.PointerCaptured;
        merged.KeyboardCaptured = merged.KeyboardCaptured || r.KeyboardCaptured;
        if (r.Hovered != kUIInvalidNode) merged.Hovered = r.Hovered;
        if (r.Focused != kUIInvalidNode) merged.Focused = r.Focused;
        if (!r.Cursor.empty()) merged.Cursor = r.Cursor;
        merged.Commands.insert(merged.Commands.end(), r.Commands.begin(), r.Commands.end());
        Dispatch(scene, it->Entity, r.Commands);
    }
    return merged;
}

// Команда документа -> событие сцены.
//
// Ровно здесь интерфейс встречается с игрой и нигде больше: узел сообщил
// строку, объект сцены знает, что она означает. Отправитель — объект с
// документом: без него обработчик «нажали» не узнает, КАКОЙ интерфейс нажали,
// и каждому пришлось бы придумывать своё имя события.
void UISceneRuntime::Dispatch(Scene& scene, entt::entity owner,
                              const std::vector<std::string>& commands) {
    if (commands.empty()) return;
    const UIDocumentComponent* c = scene.Registry().try_get<UIDocumentComponent>(owner);
    if (!c || c->Commands.empty()) return;
    const int sender = scene.Registry().all_of<IdComponent>(owner)
                           ? scene.Registry().get<IdComponent>(owner).Id
                           : 0;
    for (const std::string& cmd : commands) {
        for (const sage::events::Binding* b : sage::events::ForTrigger(c->Commands, cmd)) {
            sage::events::Event ev;
            // Имя события не задано — берём имя команды: связь, у которой
            // забыли вписать событие, должна быть заметна, а не молчать.
            ev.Name = b->Event.empty() ? cmd : b->Event;
            ev.Arg = b->Arg;
            ev.Sender = sender;
            // Адресная часть едет В ТОМ ЖЕ событии: настроить связь дважды —
            // отдельно «кому» и отдельно «что» — значит однажды поправить одно
            // и забыть другое.
            ev.Target = b->Target;
            ev.Method = b->Method;
            scene.Events.Emit(ev);
        }
    }
}

void UISceneRuntime::Render(Scene& scene, UIRenderer& renderer, glm::vec2 screenPixels,
                            Framebuffer* root) {
    for (const Open& doc : Collect(scene)) {
        UIRuntime* rt = doc.Runtime;
        rt->SetScreen(screenPixels);
        UIClassicBackend backend(renderer);
        backend.SetRootTarget(root);
        rt->Render(backend);
        const UIRenderStats& s = rt->DrawList().Stats();
        m_profile.Render.Commands += s.Commands;
        m_profile.Render.Batches += s.Batches;
        m_profile.Render.Quads += s.Quads;
        m_profile.Render.Glyphs += s.Glyphs;
        m_profile.Render.MaskPasses += s.MaskPasses;
        m_profile.Render.EffectPasses += s.EffectPasses;
        m_profile.Render.RenderTargets += s.RenderTargets;
        m_profile.Render.PrepareMs += s.PrepareMs;
    }
}

} // namespace sage::ui

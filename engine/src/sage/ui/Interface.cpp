#include "sage/ui/Interface.h"
#include "sage/assets/Pack.h"
#include "sage/ui/UIPresets.h"

#include <fstream>
#include <iterator>

#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UISerialize.h"

using json = nlohmann::json;

namespace sage::ui {
namespace {

// Версия формата ресурса. Та же дисциплина, что у сцен и сохранений: без
// номера первое же ломающее изменение молча испортило бы чужие файлы.
// Версия 2: один главный контейнер ("root") и холст ("canvas"). Файлы первой
// версии (несколько корней) больше не читаются — интерфейс того вида создают
// заново.
constexpr int kInterfaceVersion = 2;

json CanvasToJson(const Canvas& c) {
    json j;
    j["mode"] = (int)c.Mode;
    j["reference"] = {c.Reference.x, c.Reference.y};
    j["maxScale"] = c.MaxScale;
    j["match"] = c.MatchWidthOrHeight;
    return j;
}

Canvas CanvasFromJson(const json& j) {
    Canvas c;
    if (!j.is_object()) return c;
    c.Mode = (Canvas::Scale)j.value("mode", 0);
    if (j.contains("reference") && j["reference"].is_array() && j["reference"].size() == 2)
        c.Reference = {j["reference"][0].get<float>(), j["reference"][1].get<float>()};
    c.MaxScale = j.value("maxScale", 0);
    c.MatchWidthOrHeight = j.value("match", 0.5f);
    return c;
}

json NodeToJson(const InterfaceNode& n) {
    json j;
    j["name"] = n.Name;
    j["data"] = n.Data;
    if (!n.Children.empty()) {
        json kids = json::array();
        for (const InterfaceNode& c : n.Children) kids.push_back(NodeToJson(c));
        j["children"] = std::move(kids);
    }
    return j;
}

InterfaceNode NodeFromJson(const json& j) {
    InterfaceNode n;
    n.Name = j.value("name", std::string("Element"));
    if (j.contains("data") && j["data"].is_object()) n.Data = j["data"];
    if (j.contains("children") && j["children"].is_array())
        for (const json& c : j["children"]) n.Children.push_back(NodeFromJson(c));
    return n;
}

// isRoot — узел верхнего уровня ресурса. Отдельным признаком, а не «parent ==
// null»: корни ресурса теперь кладутся ВНУТРЬ объекта-интерфейса, то есть
// родитель у них есть, а корнями они быть не перестали.
void Build(Scene& scene, const InterfaceNode& node, entt::entity parent,
           std::vector<entt::entity>& roots, bool isRoot) {
    // ПУСТОЙ объект, а не CreateObject: элемент интерфейса рисует система UI, а
    // не меш, и компонент «Меш» у надписи — это секция с моделью, цветом и
    // тенями, которой нечем распорядиться.
    GameObject obj = scene.CreateEmptyObject(node.Name);
    LoadElement(node.Data, scene.Registry(), obj.Entity());
    if (parent != entt::null) scene.SetParent(obj.Entity(), parent);
    if (isRoot) roots.push_back(obj.Entity());
    for (const InterfaceNode& c : node.Children)
        Build(scene, c, obj.Entity(), roots, /*isRoot=*/false);
}

InterfaceNode Snap(const Scene& scene, entt::entity e) {
    const entt::registry& reg = scene.Registry();
    InterfaceNode n;
    if (const NameComponent* nc = reg.try_get<NameComponent>(e)) n.Name = nc->Name;
    SaveElement(n.Data, reg, e);
    if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e)) {
        for (entt::entity c : h->Children) {
            // Только элементы: 3D-объект, случайно оказавшийся внутри поддерева
            // интерфейса, в ресурс не поедет — иначе .sageui начнёт возить с
            // собой меши и материалы, которых в нём быть не должно.
            if (reg.try_get<Element>(c)) n.Children.push_back(Snap(scene, c));
        }
    }
    return n;
}

} // namespace

InterfaceNode Interface::MainContainerNode(const std::string& name) {
    // Через реестр, а не руками в JSON: запись элемента одна
    // (sage/ui/UISerialize.h), и вторая копия её формата здесь разошлась бы с
    // ней на первом же новом поле.
    entt::registry reg;
    const entt::entity e = reg.create();
    Element& el = reg.emplace<Element>(e);
    el.Type = "Container";
    el.Mode = Element::Stretch::Both;
    el.Position = {0.0f, 0.0f};
    reg.emplace<Arrangement>(e);   // свой Arrangement, как у любого контейнера
    InterfaceNode n;
    n.Name = name.empty() ? std::string("Interface") : name;
    SaveElement(n.Data, reg, e);
    return n;
}

std::string Interface::ToJsonString() const {
    json root;
    root["sage_interface_version"] = kInterfaceVersion;
    root["name"] = Name;
    root["canvas"] = CanvasToJson(Canvas);
    root["root"] = NodeToJson(Root);
    return root.dump(2);
}

bool Interface::FromJsonString(const std::string& text, Interface& out, std::string& err) {
    try {
        json root = json::parse(text);
        const int version = root.value("sage_interface_version", 0);
        if (version != kInterfaceVersion) {
            err = version > kInterfaceVersion
                      ? "интерфейс сохранён более новой версией движка (формат " + std::to_string(version) + ")"
                      : "интерфейс старого формата (" + std::to_string(version) +
                            ") — такие больше не читаются, создайте его заново";
            return false;
        }
        out = Interface{};
        out.Name = root.value("name", std::string("Interface"));
        if (root.contains("canvas")) out.Canvas = CanvasFromJson(root["canvas"]);
        if (!root.contains("root") || !root["root"].is_object()) {
            err = "в файле нет главного контейнера";
            return false;
        }
        out.Root = NodeFromJson(root["root"]);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool Interface::SaveFile(const std::string& path, std::string& err) const {
    std::ofstream f(sage::PathFromUtf8(path), std::ios::binary);
    if (!f) {
        err = "не удалось открыть на запись: " + path;
        return false;
    }
    f << ToJsonString() << '\n';
    if (!f) {
        err = "не удалось записать: " + path;
        return false;
    }
    return true;
}

bool Interface::LoadFile(const std::string& path, Interface& out, std::string& err) {
    std::string text;   // через vfs — интерфейс едет в пакете
    if (!sage::assets::vfs::ReadText(path, text)) {
        err = "нет файла: " + path;
        return false;
    }
    return FromJsonString(text, out, err);
}

std::string InterfaceFragment::ToJsonString() const {
    json root;
    root["sage_interface_fragment"] = 1;
    json nodes = json::array();
    for (const InterfaceNode& n : Nodes) nodes.push_back(NodeToJson(n));
    root["nodes"] = std::move(nodes);
    return root.dump();
}

bool InterfaceFragment::FromJsonString(const std::string& text, InterfaceFragment& out) {
    json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object() || !root.contains("nodes") ||
        !root["nodes"].is_array())
        return false;
    out = InterfaceFragment{};
    for (const json& n : root["nodes"]) out.Nodes.push_back(NodeFromJson(n));
    return true;
}

entt::entity BuildMainContainer(Scene& scene, const Interface& ui, entt::entity interfaceEntity) {
    std::vector<entt::entity> made;
    Build(scene, ui.Root, interfaceEntity, made, /*isRoot=*/true);
    return made.empty() ? entt::null : made.front();
}

entt::entity Instantiate(Scene& scene, const Interface& ui, entt::entity parent) {
    entt::registry& reg = scene.Registry();
    GameObject obj = scene.CreateEmptyObject(ui.Name.empty() ? "Interface" : ui.Name);
    InterfaceComponent& ic = reg.emplace<InterfaceComponent>(obj.Entity());
    ic.Canvas = ui.Canvas;
    if (parent != entt::null && reg.valid(parent)) scene.SetParent(obj.Entity(), parent);
    BuildMainContainer(scene, ui, obj.Entity());
    return obj.Entity();
}

Interface Capture(const Scene& scene, entt::entity interfaceEntity) {
    Interface out;
    const entt::registry& reg = scene.Registry();
    if (interfaceEntity == entt::null || !reg.valid(interfaceEntity)) return out;
    if (const NameComponent* nc = reg.try_get<NameComponent>(interfaceEntity)) out.Name = nc->Name;
    if (const InterfaceComponent* ic = reg.try_get<InterfaceComponent>(interfaceEntity))
        out.Canvas = ic->Canvas;
    // Правило «что считать главным контейнером» — одно (UISceneSystem.h).
    const entt::entity main = MainContainer(const_cast<Scene&>(scene), interfaceEntity);
    if (main != entt::null) {
        out.Root = Snap(scene, main);
        return out;
    }
    // Без главного контейнера — пустой интерфейс: содержимое всегда лежит в нём.
    out.Root = Interface::MainContainerNode(out.Name);
    return out;
}

InterfaceFragment CaptureFragment(const Scene& scene, const std::vector<entt::entity>& roots) {
    InterfaceFragment out;
    const entt::registry& reg = scene.Registry();
    for (entt::entity e : roots)
        if (reg.valid(e) && reg.all_of<Element>(e)) out.Nodes.push_back(Snap(scene, e));
    return out;
}

std::vector<entt::entity> InstantiateFragment(Scene& scene, const InterfaceFragment& frag,
                                              entt::entity parent) {
    entt::registry& reg = scene.Registry();
    // КУДА ЛЯЖЕТ ФРАГМЕНТ — зависит от того, КУДА его вставляют.
    //
    //   • в элемент — прямо туда;
    //   • в объект-интерфейс — в его главный контейнер (если он есть): всё
    //     содержимое интерфейса живёт внутри главного контейнера;
    //   • в пустое место (или в обычный объект сцены) — СВОИМ интерфейсом,
    //     иначе вставленное оказалось бы вне всех экранов и пропало из дерева.
    entt::entity container = parent;
    const bool valid = parent != entt::null && reg.valid(parent);
    if (valid && !reg.all_of<Element>(parent) && reg.all_of<InterfaceComponent>(parent)) {
        const entt::entity main = MainContainer(scene, parent);
        if (main != entt::null) container = main;
    } else if (!valid || !reg.all_of<Element>(parent)) {
        GameObject obj = scene.CreateEmptyObject("Interface");
        reg.emplace<InterfaceComponent>(obj.Entity());
        if (valid) scene.SetParent(obj.Entity(), parent);
        container = obj.Entity();
    }
    // Наружу отдаются КОРНИ. Вставка внутрь элемента корней не даёт: там
    // вставленное корнем быть перестаёт.
    const bool expose = !(valid && reg.all_of<Element>(parent));
    std::vector<entt::entity> roots;
    for (const InterfaceNode& n : frag.Nodes) Build(scene, n, container, roots, expose);
    return roots;
}

} // namespace sage::ui

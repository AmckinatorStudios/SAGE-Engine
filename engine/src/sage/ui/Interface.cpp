#include "sage/ui/Interface.h"

#include <fstream>
#include <iterator>

#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISerialize.h"

using json = nlohmann::json;

namespace sage::ui {
namespace {

// Версия формата ресурса. Та же дисциплина, что у сцен и сохранений: без
// номера первое же ломающее изменение молча испортило бы чужие файлы.
constexpr int kInterfaceVersion = 1;

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

void Build(Scene& scene, const InterfaceNode& node, entt::entity parent,
           std::vector<entt::entity>& roots) {
    // ПУСТОЙ объект, а не CreateObject: элемент интерфейса рисует система UI, а
    // не меш, и компонент «Меш» у надписи — это секция с моделью, цветом и
    // тенями, которой нечем распорядиться.
    GameObject obj = scene.CreateEmptyObject(node.Name);
    LoadElement(node.Data, scene.Registry(), obj.Entity());
    if (parent != entt::null) scene.SetParent(obj.Entity(), parent);
    else roots.push_back(obj.Entity());
    for (const InterfaceNode& c : node.Children) Build(scene, c, obj.Entity(), roots);
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

std::string Interface::ToJsonString() const {
    json root;
    root["sage_interface_version"] = kInterfaceVersion;
    root["name"] = Name;
    json roots = json::array();
    for (const InterfaceNode& n : Roots) roots.push_back(NodeToJson(n));
    root["roots"] = std::move(roots);
    return root.dump(2);
}

bool Interface::FromJsonString(const std::string& text, Interface& out, std::string& err) {
    try {
        const json root = json::parse(text);
        const int version = root.value("sage_interface_version", kInterfaceVersion);
        if (version > kInterfaceVersion) {
            err = "интерфейс сохранён более новой версией движка (формат " +
                  std::to_string(version) + ")";
            return false;
        }
        out = Interface{};
        out.Name = root.value("name", std::string("Interface"));
        if (root.contains("roots") && root["roots"].is_array())
            for (const json& n : root["roots"]) out.Roots.push_back(NodeFromJson(n));
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
    return true;
}

bool Interface::LoadFile(const std::string& path, Interface& out, std::string& err) {
    std::ifstream f(sage::PathFromUtf8(path), std::ios::binary);
    if (!f) {
        err = "нет файла: " + path;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return FromJsonString(text, out, err);
}

std::vector<entt::entity> Instantiate(Scene& scene, const Interface& ui, entt::entity parent) {
    std::vector<entt::entity> roots;
    for (const InterfaceNode& n : ui.Roots) Build(scene, n, parent, roots);
    return roots;
}

Interface Capture(const Scene& scene, const std::vector<entt::entity>& roots) {
    Interface out;
    const entt::registry& reg = scene.Registry();
    for (entt::entity e : roots) {
        if (!reg.try_get<Element>(e)) continue;
        out.Roots.push_back(Snap(scene, e));
    }
    return out;
}

} // namespace sage::ui

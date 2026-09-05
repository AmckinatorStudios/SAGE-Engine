#include "sage/ui/serialization/UIPrefab.h"

#include <fstream>
#include <sstream>

#include "sage/core/Log.h"
#include "sage/ui/core/UINode.h"
#include "sage/ui/core/UIRegistry.h"
#include "sage/ui/serialization/UISerializer.h"

namespace sage::ui {

const UIComponentType& UIPrefabInstance::StaticType() {
    static UIComponentType t = [] {
        UIComponentType d;
        d.Id = "prefab";
        d.Title = SAGE_UI_TEXT("Prefab instance");
        d.Hint = SAGE_UI_TEXT("Subtree from a reusable file plus this instance's differences");
        d.Icon = "prefab";
        d.Category = UIComponentCategory::Advanced;
        d.Order = 2;
        d.Create = [] { return std::unique_ptr<UIComponent>(new UIPrefabInstance()); };
        d.Props = {
            {"Source", SAGE_UI_TEXT("Source"), UIProperty::Kind::String,
             SAGE_UI_OFFSET(UIPrefabInstance, Source), 0, 0, nullptr, nullptr, 0,
             UIProperty::Widget::Auto, nullptr},
        };
        return d;
    }();
    return t;
}

namespace {

// Путь свойства ВНУТРИ экземпляра: "Title.text.Text". Относительный, а не
// абсолютный, ровно затем, чтобы отличия переживали переименование самого
// экземпляра и работали во вложенных префабах (§62).
std::string RelativePath(const UIDocument& doc, UINodeId root, UINodeId node) {
    std::string out;
    const UINode* n = doc.Find(node);
    while (n && n->Id != root) {
        out = out.empty() ? n->Name : n->Name + "/" + out;
        n = n->Parent == kUIInvalidNode ? nullptr : doc.Find(n->Parent);
    }
    return out;
}

UINode* ResolveRelative(UIDocument& doc, UINodeId root, const std::string& path) {
    UINode* current = doc.Find(root);
    if (path.empty()) return current;
    size_t pos = 0;
    while (current && pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string part =
            path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        UINode* next = nullptr;
        for (UINodeId cid : current->Children) {
            UINode* c = doc.Find(cid);
            if (c && c->Name == part) { next = c; break; }
        }
        current = next;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return current;
}

// Разбор "Node/Path.component.Property" на части.
bool SplitOverridePath(const std::string& full, std::string& nodePath, std::string& component,
                       std::string& property) {
    const size_t last = full.rfind('.');
    if (last == std::string::npos) return false;
    const size_t prev = full.rfind('.', last - 1);
    if (prev == std::string::npos) return false;
    nodePath = full.substr(0, prev);
    component = full.substr(prev + 1, last - prev - 1);
    property = full.substr(last + 1);
    return !component.empty() && !property.empty();
}

const UIProperty* FindProp(const UIComponent& comp, const std::string& key) {
    for (const UIProperty& p : comp.Type().Props)
        if (key == p.Key) return &p;
    return nullptr;
}

std::string ReadValue(const UIComponent& comp, const UIProperty& p) {
    if (p.Type == UIProperty::Kind::String) return UIFieldAs<std::string>(comp.Data(), p);
    std::string out;
    const int n = UIPropertyFloatCount(p);
    for (int i = 0; i < n; ++i) {
        float v = 0.0f;
        UIPropertyGetFloat(comp.Data(), p, i, v);
        if (i) out += ' ';
        out += std::to_string(v);
    }
    return out;
}

void WriteValue(UIComponent& comp, const UIProperty& p, const std::string& value) {
    if (p.Type == UIProperty::Kind::String) {
        UIFieldAs<std::string>(comp.Data(), p) = value;
        return;
    }
    std::istringstream in(value);
    const int n = UIPropertyFloatCount(p);
    for (int i = 0; i < n; ++i) {
        float v = 0.0f;
        if (!(in >> v)) break;
        UIPropertySetFloat(comp.Data(), p, i, v);
    }
}

void ApplyOverrides(UIDocument& doc, UINodeId root, const std::vector<UIPrefabOverride>& list) {
    for (const UIPrefabOverride& o : list) {
        std::string nodePath, component, property;
        if (!SplitOverridePath(o.Path, nodePath, component, property)) continue;
        UINode* node = ResolveRelative(doc, root, nodePath);
        if (!node) continue;
        UIComponent* comp = node->Find(component);
        if (!comp) comp = node->Add(component);
        if (!comp) continue;
        const UIProperty* p = FindProp(*comp, property);
        if (!p) continue;
        WriteValue(*comp, *p, o.Value);
    }
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

UINodeId UIInstantiatePrefab(UIDocument& doc, UINodeId parent, const std::string& path) {
    const std::string text = ReadFile(path);
    if (text.empty()) {
        // §134: отсутствующий префаб — предупреждение и пустой узел-заглушка с
        // сохранённой ссылкой, а не тишина. Иначе поправить путь потом не по
        // чему.
        LOG_WARN("UI") << "префаб не найден: " << path;
        UINode* stub = doc.Create("MissingPrefab", parent);
        stub->PrefabRoot = true;
        stub->PrefabSource = path;
        stub->Ensure<UIPrefabInstance>().Source = path;
        return stub->Id;
    }
    const UINodeId root = UILoadSubtree(doc, parent, text);
    if (root == kUIInvalidNode) return kUIInvalidNode;
    UINode* n = doc.Find(root);
    n->PrefabRoot = true;
    n->PrefabSource = path;
    n->Ensure<UIPrefabInstance>().Source = path;
    return root;
}

bool UICreatePrefabFromNode(UIDocument& doc, UINodeId node, const std::string& path) {
    UINode* n = doc.Find(node);
    if (!n) return false;
    // Сам компонент экземпляра в файл префаба не пишется: источник не может
    // быть экземпляром самого себя.
    const bool had = n->Find("prefab") != nullptr;
    if (had) n->RemoveById("prefab");
    const std::string text = UISaveSubtree(doc, node);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        LOG_ERROR("UI") << "не удалось записать префаб: " << path;
        return false;
    }
    out << text;
    n->PrefabRoot = true;
    n->PrefabSource = path;
    n->Ensure<UIPrefabInstance>().Source = path;
    return true;
}

int UICapturePrefabOverrides(UIDocument& doc, UINodeId instanceRoot) {
    UINode* root = doc.Find(instanceRoot);
    if (!root) return 0;
    UIPrefabInstance* inst = root->Get<UIPrefabInstance>();
    if (!inst || inst->Source.empty()) return 0;

    // Отличия считаются СРАВНЕНИЕМ с исходником, а не запоминанием правок:
    // запоминание правок промахивается на любом действии мимо редактора (отмена,
    // скрипт, миграция), а сравнение всегда честно.
    UIDocument original;
    const UILoadReport r = UILoadDocumentFromString(original, ReadFile(inst->Source));
    if (!r.Ok || original.Roots().empty()) return 0;
    const UINodeId origRoot = original.Roots().front();

    inst->Overrides.clear();
    int found = 0;
    doc.Traverse(instanceRoot, [&](UINode& node) {
        const std::string rel = RelativePath(doc, instanceRoot, node.Id);
        UINode* src = ResolveRelative(original, origRoot, rel);
        if (!src) return true;
        for (const auto& comp : node.Components) {
            const UIComponent* srcComp = src->Find(comp->Type().Id);
            if (!srcComp) continue;
            for (const UIProperty& p : comp->Type().Props) {
                const std::string mine = ReadValue(*comp, p);
                const std::string theirs = ReadValue(*srcComp, p);
                if (mine == theirs) continue;
                UIPrefabOverride o;
                o.Path = rel.empty() ? std::string(comp->Type().Id) + "." + p.Key
                                     : rel + "." + comp->Type().Id + "." + p.Key;
                o.Value = mine;
                inst->Overrides.push_back(std::move(o));
                ++found;
            }
        }
        return true;
    });
    return found;
}

int UIRefreshPrefabInstances(UIDocument& doc, const std::string& sourcePath) {
    std::vector<UINodeId> instances;
    for (UINodeId id : doc.Ordered()) {
        UINode* n = doc.Find(id);
        if (!n) continue;
        const UIPrefabInstance* inst = n->Get<UIPrefabInstance>();
        if (!inst || inst->Source.empty()) continue;
        if (!sourcePath.empty() && inst->Source != sourcePath) continue;
        instances.push_back(id);
    }

    int updated = 0;
    for (UINodeId id : instances) {
        UINode* n = doc.Find(id);
        if (!n) continue;
        UIPrefabInstance* inst = n->Get<UIPrefabInstance>();
        if (!inst) continue;
        const std::string source = inst->Source;
        const std::vector<UIPrefabOverride> overrides = inst->Overrides;
        const std::string text = ReadFile(source);
        if (text.empty()) {
            LOG_WARN("UI") << "источник префаба пропал: " << source;
            continue;
        }
        // Место в родителе сохраняется: обновление префаба не должно
        // переставлять его в конец списка.
        const UINodeId parent = n->Parent;
        int index = 0;
        if (const UINode* p = doc.Find(parent)) {
            for (int i = 0; i < (int)p->Children.size(); ++i)
                if (p->Children[(size_t)i] == id) { index = i; break; }
        }
        const std::string name = n->Name;
        doc.Destroy(id);

        const UINodeId fresh = UILoadSubtree(doc, parent, text);
        if (fresh == kUIInvalidNode) continue;
        UINode* fn = doc.Find(fresh);
        fn->Name = name;
        fn->PrefabRoot = true;
        fn->PrefabSource = source;
        UIPrefabInstance& newInst = fn->Ensure<UIPrefabInstance>();
        newInst.Source = source;
        newInst.Overrides = overrides;
        ApplyOverrides(doc, fresh, overrides);
        doc.Move(fresh, index);
        ++updated;
    }
    if (updated) doc.MarkDirty(UIDirty_All);
    return updated;
}

void UIUnpackPrefab(UIDocument& doc, UINodeId instanceRoot) {
    UINode* n = doc.Find(instanceRoot);
    if (!n) return;
    n->RemoveById("prefab");
    n->PrefabRoot = false;
    n->PrefabSource.clear();
    doc.MarkDirty(UIDirty_Hierarchy);
}

} // namespace sage::ui

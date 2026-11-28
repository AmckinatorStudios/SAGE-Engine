#include "SpriteEditorBinding.h"

#include <vector>

#include "../Localization.h"
#include "sage/scene/Scene.h"
#include "sage/render/ResourceManager.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPart.h"

namespace sage::editor::sprite {
namespace {

namespace ui = sage::ui;

const ui::PartField* FindField(const std::vector<ui::PartField>& fields, const std::string& key) {
    for (const ui::PartField& f : fields)
        if (f.Key && key == f.Key) return &f;
    return nullptr;
}

// «hoverLook.sliceBorder» -> «hoverLook.»; «sliceBorder» -> «».
std::string PrefixOf(const std::string& borderKey) {
    const std::string tail = "sliceBorder";
    if (borderKey.size() >= tail.size() &&
        borderKey.compare(borderKey.size() - tail.size(), tail.size(), tail) == 0)
        return borderKey.substr(0, borderKey.size() - tail.size());
    return {};
}

// Поля одного вида нарезки внутри части: у картинки ключи свои («path»,
// «mode»), у вида — «texture», «fit» с приставкой.
struct SliceFields {
    const ui::PartField* Border = nullptr;
    const ui::PartField* Center = nullptr;
    const ui::PartField* Edge = nullptr;
    const ui::PartField* DrawCenter = nullptr;
    const ui::PartField* Sprite = nullptr;
    const ui::PartField* Path = nullptr;
    const ui::PartField* Ref = nullptr;
    const ui::PartField* Override = nullptr;
    const ui::PartField* Fit = nullptr;
    const ui::PartField* LookHeader = nullptr;
};

SliceFields Locate(const std::vector<ui::PartField>& fields, const std::string& borderKey) {
    const std::string pre = PrefixOf(borderKey);
    SliceFields s;
    s.Border = FindField(fields, borderKey);
    s.Center = FindField(fields, pre + "sliceCenterFill");
    s.Edge = FindField(fields, pre + "sliceEdgeFill");
    s.DrawCenter = FindField(fields, pre + "sliceDrawCenter");
    s.Sprite = FindField(fields, pre + "sprite");
    s.Ref = FindField(fields, pre + "spriteRef");
    s.Override = FindField(fields, pre + "overrideSpriteSlice");
    s.Path = FindField(fields, pre + "texture");
    if (!s.Path) s.Path = FindField(fields, pre + "path");
    s.Fit = FindField(fields, pre + "fit");
    if (!s.Fit) s.Fit = FindField(fields, pre + "mode");
    if (!pre.empty()) s.LookHeader = FindField(fields, pre.substr(0, pre.size() - 1));
    return s;
}

} // namespace

bool ReadBound(EditorHost& host, const EditorHost::NineSliceTarget& target, BoundSlice& out) {
    out = BoundSlice{};
    if (target.ElementId == 0) return false;
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    GameObject obj = scene.Get(target.ElementId);
    const ui::PartType* part = ui::FindPart(target.PartId);
    if (!obj.Valid() || !part || !part->Fields || !part->Has(reg, obj.Entity())) return false;
    const void* data = part->Get(reg, obj.Entity());
    const std::vector<ui::PartField> fields = ui::EditableFields(*part->Fields);
    const SliceFields f = Locate(fields, target.BorderKey);
    if (!data || !f.Border) return false;
    out.Slice.SetBorder(ui::FieldAs<glm::vec4>(data, *f.Border));
    if (f.Center) out.Slice.CenterFill = (ui::SliceFill)ui::FieldAs<int>(data, *f.Center);
    if (f.Edge) out.Slice.EdgeFill = (ui::SliceFill)ui::FieldAs<int>(data, *f.Edge);
    if (f.DrawCenter) out.Slice.DrawCenter = ui::FieldAs<bool>(data, *f.DrawCenter);
    if (f.Sprite) out.Sprite = ui::FieldAs<glm::vec4>(data, *f.Sprite);
    if (f.Path) out.Path = ui::FieldAs<std::string>(data, *f.Path);
    if (f.Ref) {
        const auto ref = ui::FieldAs<assets::SpriteRef>(data, *f.Ref);
        if (ref.Valid()) {
            const auto resolved = ResourceManager::Instance().ResolveSprite(ref);
            const auto path = AssetDatabase::Instance().PathOf(ref.Asset);
            const std::string suffix = ".sagesheet";
            if (path.size() > suffix.size()) out.Path = path.substr(0, path.size() - suffix.size());
            if (const auto* region = resolved.Region()) {
                out.Sprite = glm::vec4(region->Rect);
                if (f.Override && !ui::FieldAs<bool>(data, *f.Override)) out.Slice = region->NineSliceOf();
            }
        }
    }
    if (const ui::Element* el = reg.try_get<ui::Element>(obj.Entity()))
        out.ElementSize = (el->Resolved.x > 0.0f && el->Resolved.y > 0.0f) ? el->Resolved : el->Size;
    out.Title = obj.Name() + "  \xe2\x80\xba  " + T(part->Title);
    if (f.LookHeader) out.Title += std::string("  \xe2\x80\xba  ") + T(f.LookHeader->Label);
    return true;
}

bool WriteBound(EditorHost& host, const EditorHost::NineSliceTarget& target, const sage::ui::NineSlice& slice) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    GameObject obj = scene.Get(target.ElementId);
    const ui::PartType* part = ui::FindPart(target.PartId);
    if (!obj.Valid() || !part || !part->Fields || !part->Has(reg, obj.Entity())) return false;
    void* data = part->GetMutable(reg, obj.Entity());
    const std::vector<ui::PartField> fields = ui::EditableFields(*part->Fields);
    const SliceFields f = Locate(fields, target.BorderKey);
    if (!data || !f.Border) return false;
    if (f.Override) ui::FieldAs<bool>(data, *f.Override) = true;
    ui::FieldAs<glm::vec4>(data, *f.Border) = slice.Border();
    if (f.Center) ui::FieldAs<int>(data, *f.Center) = (int)slice.CenterFill;
    if (f.Edge) ui::FieldAs<int>(data, *f.Edge) = (int)slice.EdgeFill;
    if (f.DrawCenter) ui::FieldAs<bool>(data, *f.DrawCenter) = slice.DrawCenter;
    // Правят девятину — значит, режим девятины: иначе числа меняются, а
    // элемент остаётся растянутым, и это выглядит как «не работает».
    if (f.Fit) ui::FieldAs<int>(data, *f.Fit) = (int)ui::Image::Mode::NineSlice;
    if (auto* element = reg.try_get<ui::Element>(obj.Entity()); element && !element->Style.empty()) {
        for (const auto* field : {f.Border, f.Center, f.Edge, f.DrawCenter, f.Fit, f.Override}) {
            if (!field) continue;
            const auto key = std::string(part->Id) + "." + field->Key;
            if (!element->Overrides(key)) element->StyleOverrides.push_back(key);
        }
    }
    return true;
}

} // namespace sage::editor::sprite

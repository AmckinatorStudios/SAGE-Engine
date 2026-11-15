// ---------------------------------------------------------------------------
// СВОЙСТВА ОБЪЕКТА-ИНТЕРФЕЙСА В СЦЕНЕ И ХОЛСТ ДОКУМЕНТА.
//
// В сцене интерфейс — один объект: файл .sageui, показан ли он, в каком
// порядке и ловит ли мышь. Его элементы здесь не правятся — их правят в
// пространстве «Интерфейс», куда ведёт кнопка «Открыть интерфейс».
// ---------------------------------------------------------------------------
#include "UIElementProperties.h"

#include "imgui.h"

#include "AssetSlot.h"
#include "EditorHost.h"
#include "InspectorControls.h"
#include "InspectorFields.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "Localization.h"

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"

namespace sage::editor {

namespace ictl = sage::editor::ictl;
namespace ifield = sage::editor::ifield;

namespace ui = sage::ui;

void OpenInInterfaceMode(EditorHost& host, GameObject obj) {
    Scene& scene = host.CurrentScene();
    const entt::entity owner = ui::InterfaceOf(scene, obj.Entity());
    if (owner == entt::null) return;
    if (const IdComponent* id = scene.Registry().try_get<IdComponent>(owner))
        host.OpenInterfaceObject(id->Id);
}

void DrawInterfaceProperties(EditorHost& host, GameObject obj) {
    entt::registry& reg = host.CurrentScene().Registry();
    ui::InterfaceComponent* info = reg.try_get<ui::InterfaceComponent>(obj.Entity());
    if (!info) return;

    // --- ИСТОЧНИК: какой файл показывает этот объект -------------------------
    assetslot::Actions actions;
    actions.Edit = [&host, id = obj.Id()] { host.OpenInterfaceObject(id); };
    const assetslot::Result r =
        assetslot::Draw(host, "##ui_source", assetslot::Kind::Interface, info->Source, nullptr,
                        T("Interface file: its elements are edited in the Interface workspace"),
                        &actions);
    if (r.Changed) {
        host.PushUndoSnapshot();
        info->Source = r.Path;   // копия развернётся заново на следующем кадре
    }

    const char* open = !info->Source.empty() ? T("Open Interface") : T("Create and open");
    if (EditorIcons::Button("ui-panel", open,
                            T("Edit the content in the Interface workspace")))
        host.OpenInterfaceObject(obj.Id());
    ImGui::Separator();

    // --- ПОДКЛЮЧЕНИЕ: как показывать весь интерфейс ---------------------------
    // Те же поля, что во всём инспекторе: «Подпись: [поле]», переключатель
    // вместо галочки, число с −/+. Снимок для отката делают сами поля.
    ifield::Check(host, T("Active"), &info->Visible);
    EditorTheme::Hint(T("off — the whole interface is hidden, with all its elements"));

    ifield::Int(host, T("Display order"), &info->SortOrder, -100, 100);
    EditorTheme::Hint(T("order BETWEEN interfaces: the HUD below the pause menu"));

    ifield::Check(host, T("Receives input"), &info->ReceivesInput);
    EditorTheme::Hint(T("off — shown but not clickable: a splash, credits, a hint over the game"));

}

void DrawInterfaceCanvas(EditorHost& host, GameObject interfaceObject) {
    if (!interfaceObject.Valid()) return;
    ui::InterfaceComponent* info =
        host.CurrentScene().Registry().try_get<ui::InterfaceComponent>(interfaceObject.Entity());
    if (!info) return;
    // Свойство ВСЕГО интерфейса, а не его элемента: верстают под один размер
    // экрана весь экран.
    const char* kModes[] = {T("Pixels"), T("Scale to reference"), T("Whole-number scale")};
    int mode = (int)info->Canvas.Mode;
    // Те же контролы, что у элемента: «Подпись: [поле]», пара W/H в строке.
    ictl::Label(T("Scale mode"));
    if (ImGui::Combo("##scale_mode", &mode, kModes, 3)) {
        host.PushUndoSnapshot();
        info->Canvas.Mode = (ui::Canvas::Scale)mode;
    }
    if (info->Canvas.Mode == ui::Canvas::Scale::Pixels) return;
    ictl::NumOpts ro;
    ro.Min = 16.0f;
    ro.Max = 8192.0f;
    ictl::PairRow(host, T("Reference resolution"), &info->Canvas.Reference.x, &info->Canvas.Reference.y, "W", "H",
                  ro);
    if (info->Canvas.Mode == ui::Canvas::Scale::ScaleWithSize) {
        ictl::SliderNumber(host, T("Match width/height"), &info->Canvas.MatchWidthOrHeight, 0.0f, 1.0f);
        EditorTheme::Hint(T("0 — follow width, 1 — follow height"));
    } else {
        ictl::Integer(host, T("Largest scale"), &info->Canvas.MaxScale, 0, 16);
        EditorTheme::Hint(T("The largest whole number that fits the reference screen: every source pixel stays the same size. 0 — no limit"));
    }
}

} // namespace sage::editor

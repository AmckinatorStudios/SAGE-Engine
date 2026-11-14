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
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "Localization.h"

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/InterfaceSource.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSystem.h"

namespace sage::editor {

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

    // Старый вид: элементы лежат прямо в сцене. Их можно только вынести в файл —
    // править их здесь значило бы держать у экрана два места правки.
    int inlineElements = 0;
    for (entt::entity c : host.CurrentScene().ChildrenOf(obj.Entity()))
        if (reg.all_of<ui::Element>(c) && !reg.all_of<ui::InterfaceInstanceTag>(c)) ++inlineElements;

    const char* open = !info->Source.empty() ? T("Open Interface")
                       : inlineElements > 0 ? T("Move to a file and open")
                                            : T("Create and open");
    if (EditorIcons::Button("ui-panel", open,
                            T("Edit the content in the Interface workspace")))
        host.OpenInterfaceObject(obj.Id());
    if (info->Source.empty() && inlineElements > 0)
        EditorTheme::Hint(T("Old-style interface: its elements live in the scene. Moving them "
                            "to a file keeps the look and makes them editable in the Interface workspace."));
    ImGui::Separator();

    // --- ПОДКЛЮЧЕНИЕ: как показывать весь интерфейс ---------------------------
    if (ImGui::Checkbox(T("Active"), &info->Visible)) host.PushUndoSnapshot();
    EditorTheme::Hint(T("off — the whole interface is hidden, with all its elements"));

    ImGui::DragInt(T("Display order"), &info->SortOrder, 0.1f, -100, 100);
    host.TrackLastImGuiItem();
    EditorTheme::Hint(T("order BETWEEN interfaces: the HUD below the pause menu"));

    if (ImGui::Checkbox(T("Receives input"), &info->ReceivesInput)) host.PushUndoSnapshot();
    EditorTheme::Hint(T("off — shown but not clickable: a splash, credits, a hint over the game"));

    // Холст интерфейса из файла едет вместе с файлом и правится там же; у
    // старого вида он живёт в сцене — показываем его здесь, как раньше.
    if (info->Source.empty() && inlineElements > 0) {
        ImGui::SeparatorText(T("Canvas"));
        DrawInterfaceCanvas(host, obj);
    }
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
    if (ImGui::Combo(T("Scale mode"), &mode, kModes, 3)) {
        host.PushUndoSnapshot();
        info->Canvas.Mode = (ui::Canvas::Scale)mode;
    }
    if (info->Canvas.Mode == ui::Canvas::Scale::Pixels) return;
    float ref[2] = {info->Canvas.Reference.x, info->Canvas.Reference.y};
    if (ImGui::DragFloat2(T("Reference resolution"), ref, 1.0f, 16.0f, 8192.0f, "%.0f"))
        info->Canvas.Reference = {ref[0], ref[1]};
    host.TrackLastImGuiItem();
    if (info->Canvas.Mode == ui::Canvas::Scale::ScaleWithSize) {
        ImGui::SliderFloat(T("Match width/height"), &info->Canvas.MatchWidthOrHeight, 0.0f, 1.0f);
        host.TrackLastImGuiItem();
        EditorTheme::Hint(T("0 — follow width, 1 — follow height"));
    } else {
        ImGui::DragInt(T("Largest scale"), &info->Canvas.MaxScale, 0.05f, 0, 16);
        host.TrackLastImGuiItem();
        EditorTheme::Hint(T("The largest whole number that fits the reference screen: every source pixel stays the same size. 0 — no limit"));
    }
}

} // namespace sage::editor

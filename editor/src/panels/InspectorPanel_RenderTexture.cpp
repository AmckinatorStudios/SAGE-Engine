// ---------------------------------------------------------------------------
// RENDER TEXTURE в инспекторе: съёмка сцены с этого объекта в именованную
// картинку («rt:<имя>»). Её показывает Image интерфейса (Source → Render
// Texture) или материал: превью персонажа, объёмная иконка предмета, экран
// камеры наблюдения.
// ---------------------------------------------------------------------------
#include "InspectorPanel.h"

#include "imgui.h"

#include "EditorHost.h"
#include "EditorTheme.h"
#include "../Localization.h"
#include "ui/ColorPicker.h"

#include "sage/scene/Components.h"

void DrawRenderTextureSection(EditorHost& host, GameObject obj, entt::registry& reg, bool* remove) {
    if (!reg.all_of<RenderTextureComponent>(obj.Entity())) return;
    if (!EditorTheme::SectionHeader("camera", T("Render Texture" "###Render Texture"), ImGuiTreeNodeFlags_DefaultOpen,
                                    remove, T("Films the scene from this object into a picture named rt:<name>")))
        return;
    RenderTextureComponent& rt = reg.get<RenderTextureComponent>(obj.Entity());
    bool changed = false;

    char name[128];
    std::snprintf(name, sizeof(name), "%s", rt.Target.c_str());
    if (ImGui::InputText(T("Name"), name, sizeof(name))) {
        rt.Target = name;
        changed = true;
    }
    host.TrackLastImGuiItem();
    EditorTheme::Hint(T("An Image shows it with Source: Render Texture and this name"));

    // Разрешение — степенями двойки и своим числом: превью в меню редко
    // нужно больше 512, а лишние пиксели — это стоимость каждой съёмки.
    int size[2] = {rt.Width, rt.Height};
    if (ImGui::DragInt2(T("Resolution"), size, 1.0f, 16, 4096)) {
        rt.Width = std::clamp(size[0], 16, 4096);
        rt.Height = std::clamp(size[1], 16, 4096);
        changed = true;
    }
    host.TrackLastImGuiItem();
    if (ImGui::DragFloat(T("Aspect ratio"), &rt.Aspect, 0.01f, 0.0f, 8.0f, "%.3f")) changed = true;
    host.TrackLastImGuiItem();
    if (rt.Aspect > 0.0f) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d x %d", rt.Width, rt.TargetHeight());
        EditorTheme::Hint((std::string(T("Picture size: ")) + buf).c_str());
    } else {
        EditorTheme::Hint(T("Width divided by height; 0 — use the resolution as set"));
    }

    bool transparent = rt.ClearColor.a < 0.5f;
    if (ImGui::Checkbox(T("Transparent background"), &transparent)) {
        host.PushUndoSnapshot();
        rt.ClearColor.a = transparent ? 0.0f : 1.0f;
        changed = true;
    }
    if (!transparent) {
        if (Sage::UI::ColorField4(T("Background"), &rt.ClearColor.x)) changed = true;
        host.TrackLastImGuiItem();
    }

    const char* kModes[] = {T("On Demand"), T("On Change"), T("Continuous")};
    int mode = rt.Continuous ? 2 : (int)rt.Mode;
    if (ImGui::Combo(T("Update"), &mode, kModes, 3)) {
        host.PushUndoSnapshot();
        rt.Mode = (RenderTextureComponent::Update)mode;
        rt.Continuous = false;
        changed = true;
    }
    EditorTheme::Hint(mode == 0   ? T("Once, and again when a script calls rt.Refresh")
                      : mode == 1 ? T("When the camera or the objects in view move")
                                  : T("Every frame — a full scene pass each time, use sparingly"));

    if (ImGui::Checkbox(T("Orthographic"), &rt.Ortho)) changed = true;
    if (rt.Ortho) {
        if (ImGui::DragFloat(T("Half height"), &rt.OrthoSize, 0.05f, 0.01f, 1000.0f)) changed = true;
    } else if (ImGui::SliderFloat(T("Field of view"), &rt.Fov, 5.0f, 150.0f, "%.0f°")) {
        changed = true;
    }
    host.TrackLastImGuiItem();
    if (ImGui::DragFloat3(T("Look at"), &rt.LookAt.x, 0.05f)) changed = true;
    host.TrackLastImGuiItem();
    if (EditorTheme::BeginMore("rtMore")) {
        if (ImGui::DragFloat(T("Near"), &rt.Near, 0.01f, 0.001f, 100.0f)) changed = true;
        host.TrackLastImGuiItem();
        if (ImGui::DragFloat(T("Far"), &rt.Far, 1.0f, 1.0f, 100000.0f)) changed = true;
        host.TrackLastImGuiItem();
        if (ImGui::Checkbox(T("Studio light"), &rt.StudioLight)) changed = true;
        EditorTheme::Hint(T("Even light of its own: an item icon looks the same at noon and at night"));
        if (rt.StudioLight) {
            if (ImGui::SliderFloat(T("Light intensity"), &rt.LightIntensity, 0.0f, 5.0f)) changed = true;
            host.TrackLastImGuiItem();
            if (ImGui::SliderFloat(T("Ambient"), &rt.Ambient, 0.0f, 2.0f)) changed = true;
            host.TrackLastImGuiItem();
        }
        EditorTheme::EndMore();
    }
    if (ImGui::Button(T("Refresh now"))) rt.Dirty = true;
    // Любая правка — снять заново: картинка снята под прежние числа.
    if (changed) rt.Dirty = true;
}

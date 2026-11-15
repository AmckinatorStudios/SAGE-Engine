// ---------------------------------------------------------------------------
// ОБРАЗЕЦ ИНТЕРФЕЙСА ДЛЯ СНИМКОВ (SAGE_EDITOR_UI_SAMPLE).
//
// Визуальная проверка элементов — глазами по снимку редактора (CLAUDE.md):
// документ интерфейса со всеми видами элементов в заметных состояниях
// (раскрытый список, выключатель, круговая шкала…). Только через переменную
// окружения: в меню и окнах редактора этого нет.
// ---------------------------------------------------------------------------
#include <cstdlib>
#include <string>

#include <entt/entt.hpp>
#include <stb_image_write.h>

#include "EditorLayer.h"
#include "sage/core/Paths.h"
#include "sage/scene/Components.h"
#include "sage/ui/SpriteSheet.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIStyle.h"

#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIButton.h"
#include "sage/ui/UIPresets.h"

namespace sage::editor {
void FillUISample(Scene& scene, entt::entity root, const std::string& which);

namespace {
entt::entity Put(Scene& scene, entt::entity parent, const char* preset, const char* name, glm::vec2 pos,
                 glm::vec2 size) {
    GameObject o = scene.CreateEmptyObject(name);
    scene.SetParent(o.Entity(), parent);
    sage::ui::ApplyPreset(scene, o.Entity(), preset);
    sage::ui::Element& el = scene.Registry().get<sage::ui::Element>(o.Entity());
    el.Anchor = UIAnchor::TopLeft;
    el.Position = pos;
    el.Size = size;
    return o.Entity();
}
} // namespace

void FillUISample(Scene& scene, entt::entity root, const std::string& which) {
    namespace ui = sage::ui;
    entt::registry& reg = scene.Registry();
    if (root == entt::null) return;
    (void)which;
    const entt::entity button = Put(scene, root, "Button", "Play", {40, 40}, {220, 56});
    ui::ButtonContent bc;
    bc.Label = "Play";
    bc.Icon = "play";
    ui::ApplyButtonContent(scene, button, bc);
    Put(scene, root, "Button", "Settings", {40, 110}, {220, 56});

    const entt::entity sw = Put(scene, root, "Toggle", "Music", {40, 190}, {220, 36});
    reg.get<ui::Range>(sw).ToggleVariant = ui::Range::Variant::Switch;
    reg.get<ui::Range>(sw).Value = 1.0f;
    ui::PlaceToggleCaption(scene, sw);
    Put(scene, root, "Toggle", "Subtitles", {40, 236}, {220, 36});
    Put(scene, root, "Slider", "Volume", {40, 290}, {220, 30});
    const entt::entity vs = Put(scene, root, "Slider", "Level", {280, 40}, {30, 200});
    reg.get<ui::Range>(vs).Vertical = true;
    reg.get<ui::Range>(vs).Value = 0.7f;

    const entt::entity input = Put(scene, root, "Text Input", "Name", {40, 340}, {260, 40});
    reg.get<ui::TextInput>(input).Placeholder = "Player name";
    const entt::entity dd = Put(scene, root, "Dropdown", "Quality", {340, 40}, {240, 40});
    auto& d = reg.get<ui::Dropdown>(dd);
    d.Options = "Low\nMedium\nHigh\nUltra";
    d.Disabled = "Ultra";
    d.Selected = 1;
    d.Open = true;
    d.Hover = 2;

    const entt::entity lin = Put(scene, root, "Progress", "Health", {340, 260}, {240, 26});
    reg.get<ui::Bar>(lin).Value = 0.65f;
    const entt::entity rad = Put(scene, root, "Progress", "Cooldown", {620, 40}, {120, 120});
    auto& r = reg.get<ui::Bar>(rad);
    r.Fill = ui::Bar::Kind::Radial;
    r.RingWidth = 14.0f;
    r.Value = 0.7f;
    const entt::entity pie = Put(scene, root, "Progress", "Pie", {760, 40}, {120, 120});
    auto& p = reg.get<ui::Bar>(pie);
    p.Fill = ui::Bar::Kind::Radial;
    p.FillAmount = 0.75f;
    p.StartAngle = -135.0f;
    p.Value = 0.4f;
    const entt::entity path = Put(scene, root, "Progress", "Route", {620, 200}, {260, 100});
    auto& pa = reg.get<ui::Bar>(path);
    pa.Fill = ui::Bar::Kind::Path;
    pa.Path = "M 0 0.8 C 0.3 -0.2 0.7 1.2 1 0.2";
    pa.PathWidth = 10.0f;
    pa.Value = 0.6f;
}

} // namespace sage::editor

using sage::editor::FillUISample;

// Какой образец: all — все элементы; rt — объект с Render Texture в сцене;
// image-rt — картинка интерфейса с источником Render Texture; sprite — лист
// спрайтов с последовательностью; style — элемент со стилем и
// переопределением; anim — образец и инструмент анимации.
void EditorLayer::RunUISampleHook(const std::string& sample) {
    namespace ui = sage::ui;
    if (sample == "rt" || sample == "image-rt") {
        GameObject cam = m_scene->CreateEmptyObject("Portrait camera");
        RenderTextureComponent rt;
        rt.Target = "portrait";
        rt.Mode = RenderTextureComponent::Update::OnChange;
        rt.Aspect = 0.75f;
        m_scene->Registry().emplace<RenderTextureComponent>(cam.Entity(), rt);
        if (sample == "rt") {
            m_selection.SetPrimary(cam.Id());
            return;
        }
    }
    if (sample.rfind("sprite", 0) == 0) {
        // Лист 160 x 32: четыре кадра монетки разной ширины (видно, что кадры
        // листаются) и рамка-панель 32 x 32 с уголками по 8 пикселей — её
        // девятину показывает режим Nine-Slice.
        const std::filesystem::path dir = m_project.AssetsDir() / "ui";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const int W = 160, H = 32;
        std::vector<unsigned char> px((size_t)W * H * 4, 0);
        for (int f = 0; f < 4; ++f)
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x) {
                    const float rx = 4.0f + 10.0f * (float)(f == 0 || f == 2 ? 1 : (f == 1 ? 0.5f : 0.2f));
                    const float dx = (float)x - 15.5f, dy = (float)y - 15.5f;
                    if (dx * dx / (rx * rx) + dy * dy / 196.0f > 1.0f) continue;
                    unsigned char* p = &px[((size_t)y * W + (size_t)(f * 32 + x)) * 4];
                    p[0] = 250; p[1] = 200; p[2] = 60; p[3] = 255;
                }
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const bool corner = (x < 3 || x > 28) && (y < 3 || y > 28);
                if (corner) continue;
                const bool border = x < 4 || x > 27 || y < 4 || y > 27;
                unsigned char* p = &px[((size_t)y * W + (size_t)(128 + x)) * 4];
                p[0] = border ? 90 : 40; p[1] = border ? 170 : 60; p[2] = border ? 255 : 110; p[3] = 255;
            }
        const std::string file = sage::PathToUtf8(dir / "coin.png");
        stbi_write_png(file.c_str(), W, H, 4, px.data(), W * 4);
        ui::SpriteSheet sheet;
        ui::SpriteGrid grid;
        grid.CellW = grid.CellH = 32;
        grid.Prefix = "coin";
        if (sample == "sprite-grid") {
            // Без кусков, клетка 48 x 32: последняя клетка в строке выходит
            // за картинку — живой предпросмотр красит её красным.
            grid.CellW = 48;
            sheet.Slicing.Grid = grid;
        } else if (sample != "sprite-empty") {
            sheet.Slicing.Grid = grid;
            sheet.SliceGrid(W, H, grid, px.data());
            sheet.Regions[0].Pivot = {0.5f, 1.0f};
            sheet.Rename("coin_4", "panel");
            if (ui::SpriteRegion* panel = sheet.Find("panel")) panel->Slice = glm::vec4(8.0f);
            sheet.SequenceFromPrefix("spin", "coin_", 8.0f);
        }
        std::string err;
        if (sample != "sprite-empty") sheet.SaveFile(ui::SpriteSheet::SidecarPath(file), err);
        m_panelRegistry.SetVisible("SpriteSheet", true);
        if (sample == "sprite-empty") return;
        const bool nine = sample == "sprite-nine" || sample == "sprite-result";
        m_spriteSheet.Open(m_project.AssetRef(dir / "coin.png"), nine ? 4 : (sample == "sprite-grid" ? -1 : 0), 0);
        m_spriteSheet.ShowNineSlice(nine);
        m_spriteSheet.ShowNineResult(sample == "sprite-result");
        if (sample == "sprite-replace") m_spriteSheet.CreateSpritesNow();
        return;
    }
    SetWorkspace(EditorWorkspace::Interface);
    NewInterfaceDocument();
    GameObject iface = m_scene->Get(m_currentInterface);
    if (!iface.Valid()) return;
    const entt::entity root = ui::MainContainer(*m_scene, iface.Entity());
    FillUISample(*m_scene, root, sample);
    // Свойства выбранного — вперёд холста: на снимке нужно видеть инспектор.
    if (sample != "all" && sample != "anim") {
        m_focusPanelId = "InterfaceInspector";
        m_focusPanelFrames = 60;
    }
    // Выбрать элемент образца по имени — снять инспектор нужного типа.
    if (const std::string pick = sage::EnvString("SAGE_EDITOR_UI_SELECT"); !pick.empty()) {
        if (GameObject o = m_scene->FindByName(pick); o.Valid()) {
            m_selection.SetPrimary(o.Id());
            // Своё оформление — снять вложенные группы вида (квадратик,
            // источник картинки, скругление), которых у оформления движка нет.
            if (std::getenv("SAGE_EDITOR_UI_CUSTOM")) {
                entt::registry& reg = m_scene->Registry();
                if (ui::Element* el = reg.try_get<ui::Element>(o.Entity())) {
                    ui::BakeEngineSkin(reg, o.Entity());
                    el->Skin = ui::Element::SkinMode::Custom;
                }
            }
            // Девятина подложки из инспектора — в редакторе спрайтов: рамка
            // 32 x 32 с углами по 6 пикселей картинкой подложки.
            if (std::getenv("SAGE_EDITOR_UI_NINESLICE")) {
                const std::filesystem::path dir = m_project.AssetsDir() / "ui";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                std::vector<unsigned char> px(32 * 32 * 4, 0);
                for (int y = 0; y < 32; ++y)
                    for (int x = 0; x < 32; ++x) {
                        const bool border = x < 6 || x > 25 || y < 6 || y > 25;
                        unsigned char* p = &px[((size_t)y * 32 + (size_t)x) * 4];
                        p[0] = border ? 240 : 40; p[1] = border ? 170 : 50; p[2] = border ? 60 : 80; p[3] = 255;
                    }
                stbi_write_png(sage::PathToUtf8(dir / "frame.png").c_str(), 32, 32, 4, px.data(), 32 * 4);
                if (ui::Fill* fill = m_scene->Registry().try_get<ui::Fill>(o.Entity())) {
                    fill->Texture = m_project.AssetRef(dir / "frame.png");
                    fill->SliceBorder = glm::vec4(6.0f);
                    fill->Fit = 1;   // девятина
                }
                EditorHost::NineSliceTarget t;
                t.ElementId = o.Id();
                t.PartId = "fill";
                t.BorderKey = "sliceBorder";
                OpenNineSliceEditor(t);
            }
            m_focusPanelId = "InterfaceInspector";
            m_focusPanelFrames = 60;
        }
    }
    if (sample == "image-rt") {
        GameObject view = m_scene->CreateEmptyObject("Portrait view");
        m_scene->SetParent(view.Entity(), root);
        ui::ApplyPreset(*m_scene, view.Entity(), "Image");
        auto& el = m_scene->Registry().get<ui::Element>(view.Entity());
        el.Anchor = UIAnchor::TopLeft;
        el.Position = {940, 40};
        el.Size = {150, 200};
        m_scene->Registry().get<ui::Image>(view.Entity()).Path = "rt:portrait";
        m_selection.SetPrimary(view.Id());
    } else if (sample == "style") {
        const std::filesystem::path dir = m_project.AssetsDir() / "styles";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        GameObject play = m_scene->FindByName("Settings");
        entt::registry& reg = m_scene->Registry();
        const std::string file = sage::PathToUtf8(dir / "menu.sageuistyle");
        ui::SaveStyleFile(file, ui::CaptureStyle(reg, play.Entity()));
        ui::Element& el = reg.get<ui::Element>(play.Entity());
        el.Style = m_project.AssetRef(dir / "menu.sageuistyle");
        el.StyleOverrides.push_back("fill.color");
        m_selection.SetPrimary(play.Id());
    } else if (sample == "anim") {
        GameObject play = m_scene->FindByName("Play");
        m_selection.SetPrimary(play.Id());
        m_panels[EditorPanel::Animation] = true;
        m_animation.RequestFocus(60);
    }
}

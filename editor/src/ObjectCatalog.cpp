#include "ObjectCatalog.h"

#include <cstring>

#include <imgui.h>

#include "EditorIcons.h"
#include "Localization.h"

namespace sage::editor::objectcatalog {

Group Catalog() {
    Group root;
    // ПУСТОЙ ОБЪЕКТ — ОТДЕЛЬНЫМ ПУНКТОМ, а не в категории. Его берут чаще
    // любого другого (узел иерархии, точка привязки), и прятать его на второй
    // уровень значило бы сделать самое частое действие самым долгим.
    root.Items = {
        {"empty", T("Empty Object"), T("Only a name and a transform — a node, a pivot, a script holder")},
    };

    root.Groups = {
        {T("Shapes"),
         {
             {"shape.cube",     T("Cube"),     T("Box: walls, floors, crates")},
             {"shape.sphere",   T("Sphere"),   T("Ball: projectiles, markers")},
             {"shape.plane",    T("Plane"),    T("Flat square: ground, water surface")},
             {"shape.cylinder", T("Cylinder"), T("Column, barrel, pipe")},
             {"shape.cone",     T("Cone"),     T("Spike, roof, direction marker")},
             {"shape.capsule",  T("Capsule"),  T("Cylinder with rounded ends: the working shape of a character")},
         },
         {}},

        {T("Light"),
         {
             {"light.point", T("Point light"), T("Glows in all directions: lamp, fire, torch")},
             {"light.spot",  T("Spotlight"),   T("Cone of light: flashlight, street lamp")},
             {"light.sun",   T("Sun"),         T("Parallel light for the whole scene, casts the main shadows")},
         },
         {}},

        {T("Camera"),
         {
             {"camera.game", T("Game camera"), T("The Game panel and the built game look through it")},
         },
         {}},

        {T("Physics"),
         {
             {"physics.box",      T("Falling crate"),    T("Cube with mass and a collider — falls and stacks")},
             {"physics.platform", T("Static platform"),  T("Motionless floor others land on")},
             {"physics.trigger",  T("Trigger zone"),     T("Invisible volume: reports entering and leaving, does not block")},
             {"physics.character",T("Character"),        T("Capsule with a controller: walks, climbs steps, jumps")},
         },
         {}},

        {T("Effects"),
         {
             {"fx.decal",      T("Decal"),            T("Projects a texture onto surfaces: cracks, stains, posters")},
             {"fx.probe",      T("Reflection probe"), T("Captures the surroundings so nearby surfaces reflect them")},
         },
         {
             // Один пункт, а не «огонь / дым / искры»: готовых эффектов в
             // движке нет — эффект собирается в инспекторе или приходит файлом
             // .sagefx (его можно бросить на объект из дерева проекта).
             {T("Particles"),
              {
                  {"fx.particles", T("Particle system"), T("An emitter to shape into any effect: rain, snow, smoke, fire, magic")},
              },
              {}},
         }},

        {T("Sound"),
         {
             {"audio.source", T("Sound source"), T("Sounds from this point: ambience, hum, one-shot")},
         },
         {}},

        {T("Logic"),
         {
             {"logic.script", T("Script object"), T("Empty object with a Lua script: OnStart and OnUpdate")},
         },
         {}},

        {T("Animation"),
         {
             {"anim.model", T("Animated model"), T("Mesh plus Animation: skeleton and clips come from the model")},
         },
         {}},

        {T("Interface"),
         {},
         {
             {T("Elements"),
              {
                  {"ui.Panel",    T("Panel"),    T("Background block: groups other elements")},
                  {"ui.Button",   T("Button"),   T("Backing, caption and a reaction to the mouse")},
                  {"ui.Label",    T("Label"),    T("Line of text")},
                  {"ui.Image",    T("Image"),    T("Picture from the project")},
                  {"ui.Bar",      T("Bar"),      T("Fill level: health, loading, progress")},
                  {"ui.Checkbox", T("Checkbox"), T("On/off switch")},
                  {"ui.Slider",   T("Slider"),   T("Value picked by dragging")},
                  {"ui.Input",    T("Input"),    T("Text field")},
              },
              {}},
             {T("Screens"),
              {
                  {"ui.screen.menu",     T("Main menu"),  T("Ready screen: title and a column of buttons")},
                  {"ui.screen.hud",      T("HUD"),        T("Health, ammo and a crosshair over the game")},
                  {"ui.screen.settings", T("Settings"),   T("Sound, quality and controls on one screen")},
              },
              {}},
         }},
    };
    return root;
}

namespace {

// ЗНАЧОК ПУНКТА — ПО ЕГО КЛЮЧУ, а не по подписи.
//
// Ключ («shape.cube», «light.spot») не переводится и не меняется от языка, а
// подпись — и то и другое. Заводить ещё одно поле в таблице каталога ради
// рисунка не нужно: ключ уже говорит, что это за объект, и ровно на этом
// знании таблица и построена.
const char* IconForId(const char* id) {
    if (!id) return "cube";
    struct Row { const char* Id; const char* Icon; };
    // Точные совпадения — там, где внутри семейства рисунки разные.
    static const Row kExact[] = {
        {"shape.sphere", "sphere"},   {"shape.cone", "cone"},
        {"light.sun", "sun"},         {"light.point", "light"}, {"light.spot", "cone"},
        {"fx.probe", "probe"},        {"fx.decal", "texture"},
        {"ui.Panel", "ui-panel"},     {"ui.Button", "ui-button"},
        {"ui.Label", "ui-text"},      {"ui.Image", "ui-image"},
        {"ui.Bar", "ui-bar"},         {"ui.Checkbox", "ui-check"},
        {"ui.Slider", "ui-slider"},   {"ui.Input", "ui-input"},
    };
    for (const Row& r : kExact) {
        if (std::strcmp(id, r.Id) == 0) return r.Icon;
    }
    // Семейство целиком — по началу ключа.
    static const Row kPrefix[] = {
        {"shape.", "cube"},   {"light.", "light"},  {"camera.", "camera"},
        {"logic.", "script"}, {"audio.", "audio"},  {"anim.", "anim"},
        {"physics.", "physics"}, {"fx.", "particles"}, {"ui.screen.", "ui-screen"},
        {"ui.", "ui-empty"},
    };
    for (const Row& r : kPrefix) {
        if (std::strncmp(id, r.Id, std::strlen(r.Id)) == 0) return r.Icon;
    }
    return "cube";
}

// Значок СПИСКА — значок его первого пункта: «Фигуры» открываются кубом,
// «Свет» — лампой. Своё поле под это завело бы вторую таблицу, которая
// однажды разойдётся с первой.
const char* IconForGroup(const Group& g) {
    if (!g.Items.empty()) return IconForId(g.Items.front().Id);
    if (!g.Groups.empty()) return IconForGroup(g.Groups.front());
    return "folder";
}

// Рисует пункты и вложенные списки. Первый выбранный Id останавливает разбор:
// два пункта за один кадр нажать нельзя, а проверять дальше — значит рисовать
// уже ненужное.
void DrawGroup(const Group& g, const char*& picked) {
    for (const Item& it : g.Items) {
        if (EditorIcons::MenuItem(IconForId(it.Id), it.Label)) picked = it.Id;
        // Подсказка — про НАЗНАЧЕНИЕ, а не про название. Список из сорока
        // пунктов без неё требует пробовать каждый, чтобы понять, чем
        // «Плоскость» отличается от «Куба» в деле.
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.Hint);
    }
    for (const Group& sub : g.Groups) {
        if (!EditorIcons::BeginMenu(IconForGroup(sub), sub.Label)) continue;
        DrawGroup(sub, picked);
        ImGui::EndMenu();
    }
}

} // namespace

const char* DrawMenu() {
    const Group root = Catalog();
    const char* picked = nullptr;
    for (const Item& it : root.Items) {
        if (EditorIcons::MenuItem(IconForId(it.Id), it.Label)) picked = it.Id;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.Hint);
    }
    if (!root.Items.empty() && !root.Groups.empty()) ImGui::Separator();
    for (const Group& g : root.Groups) {
        if (!EditorIcons::BeginMenu(IconForGroup(g), g.Label)) continue;
        DrawGroup(g, picked);
        ImGui::EndMenu();
    }
    return picked;
}

} // namespace sage::editor::objectcatalog

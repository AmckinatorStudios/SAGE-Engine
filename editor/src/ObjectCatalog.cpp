#include "ObjectCatalog.h"

#include <imgui.h>

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
             {T("Particles"),
              {
                  {"fx.particles.fire",    T("Fire"),        T("Flame tongues with an upward draft")},
                  {"fx.particles.smoke",   T("Smoke"),       T("Slow grey plume")},
                  {"fx.particles.sparks",  T("Sparks"),      T("Short hot flecks: grinding, shorts")},
                  {"fx.particles.splash",  T("Water splash"),T("Spray of droplets")},
                  {"fx.particles.embers",  T("Embers"),      T("Rare glowing specks over a fire")},
                  {"fx.particles.debris",  T("Debris"),      T("Shards of a broken block")},
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

// Рисует пункты и вложенные списки. Первый выбранный Id останавливает разбор:
// два пункта за один кадр нажать нельзя, а проверять дальше — значит рисовать
// уже ненужное.
void DrawGroup(const Group& g, const char*& picked) {
    for (const Item& it : g.Items) {
        if (ImGui::MenuItem(it.Label)) picked = it.Id;
        // Подсказка — про НАЗНАЧЕНИЕ, а не про название. Список из сорока
        // пунктов без неё требует пробовать каждый, чтобы понять, чем
        // «Плоскость» отличается от «Куба» в деле.
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.Hint);
    }
    for (const Group& sub : g.Groups) {
        if (!ImGui::BeginMenu(sub.Label)) continue;
        DrawGroup(sub, picked);
        ImGui::EndMenu();
    }
}

} // namespace

const char* DrawMenu() {
    const Group root = Catalog();
    const char* picked = nullptr;
    for (const Item& it : root.Items) {
        if (ImGui::MenuItem(it.Label)) picked = it.Id;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.Hint);
    }
    if (!root.Items.empty() && !root.Groups.empty()) ImGui::Separator();
    for (const Group& g : root.Groups) {
        if (!ImGui::BeginMenu(g.Label)) continue;
        DrawGroup(g, picked);
        ImGui::EndMenu();
    }
    return picked;
}

} // namespace sage::editor::objectcatalog

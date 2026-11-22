#include "GameIcons.h"

#include "sage/ui/UIIcons.h"

#include <glm/glm.hpp>

// Формы описаны в НОРМАЛИЗОВАННЫХ координатах 0..1 внутри своего квадрата —
// см. sage::ui::IconPen. Одна и та же иконка одинаково резка и в 16, и в 64
// пикселя, а цвет приходит параметром отрисовки.
namespace {

using sage::ui::IconPen;

void IconFood(const IconPen& p) { // кусок с косточкой — «сытость»
    p.Dot(0.5f, 0.55f, 0.30f);
    IconPen inner = p;
    inner.Color = glm::mix(p.Color, glm::vec3(0.0f), 0.35f);
    inner.Line(0.34f, 0.72f, 0.66f, 0.72f, 0.10f);
}
void IconBreath(const IconPen& p) { // пузырьки воздуха
    p.Ring(0.42f, 0.55f, 0.26f, 0.09f);
    p.Ring(0.72f, 0.28f, 0.13f, 0.08f);
    p.Dot(0.84f, 0.62f, 0.06f);
}
void IconWave(const IconPen& p) {
    p.Line(0.10f, 0.40f, 0.32f, 0.28f, 0.11f);
    p.Line(0.32f, 0.28f, 0.54f, 0.40f, 0.11f);
    p.Line(0.54f, 0.40f, 0.76f, 0.28f, 0.11f);
    p.Line(0.76f, 0.28f, 0.92f, 0.36f, 0.11f);
    p.Line(0.10f, 0.68f, 0.32f, 0.56f, 0.11f);
    p.Line(0.32f, 0.56f, 0.54f, 0.68f, 0.11f);
    p.Line(0.54f, 0.68f, 0.76f, 0.56f, 0.11f);
    p.Line(0.76f, 0.56f, 0.92f, 0.64f, 0.11f);
}
void IconCompass(const IconPen& p) {
    p.Ring(0.5f, 0.5f, 0.40f, 0.08f);
    p.Tri(0.5f, 0.20f, 0.62f, 0.56f, 0.38f, 0.56f);
    p.Dot(0.5f, 0.62f, 0.07f);
}
void IconPlank(const IconPen& p) {
    p.Box(0.10f, 0.30f, 0.80f, 0.18f, 0.05f);
    p.Box(0.10f, 0.54f, 0.80f, 0.18f, 0.05f);
}
void IconLog(const IconPen& p) {
    p.Box(0.08f, 0.34f, 0.84f, 0.32f, 0.14f);
    IconPen inner = p;
    inner.Color = glm::mix(p.Color, glm::vec3(0.0f), 0.35f);
    inner.Ring(0.80f, 0.50f, 0.14f, 0.07f);
}
void IconCrate(const IconPen& p) {
    p.Box(0.12f, 0.18f, 0.76f, 0.64f, 0.07f);
    IconPen inner = p;
    inner.Color = glm::mix(p.Color, glm::vec3(0.0f), 0.4f);
    inner.Line(0.12f, 0.18f, 0.88f, 0.82f, 0.09f);
    inner.Line(0.88f, 0.18f, 0.12f, 0.82f, 0.09f);
}
void IconBarrel(const IconPen& p) {
    p.Box(0.20f, 0.14f, 0.60f, 0.72f, 0.22f);
    IconPen inner = p;
    inner.Color = glm::mix(p.Color, glm::vec3(0.0f), 0.4f);
    inner.Box(0.18f, 0.32f, 0.64f, 0.08f, 0.03f);
    inner.Box(0.18f, 0.60f, 0.64f, 0.08f, 0.03f);
}
void IconRope(const IconPen& p) {
    p.Ring(0.5f, 0.5f, 0.36f, 0.10f);
    p.Ring(0.5f, 0.5f, 0.19f, 0.09f);
}
void IconCloth(const IconPen& p) {
    p.Quad4(0.14f, 0.22f, 0.86f, 0.14f, 0.86f, 0.72f, 0.14f, 0.84f);
    IconPen inner = p;
    inner.Color = glm::mix(p.Color, glm::vec3(0.0f), 0.3f);
    inner.Line(0.14f, 0.53f, 0.86f, 0.43f, 0.06f);
}
void IconPlastic(const IconPen& p) { // бутылка
    p.Box(0.36f, 0.10f, 0.28f, 0.16f, 0.05f);
    p.Box(0.26f, 0.26f, 0.48f, 0.62f, 0.14f);
}
void IconFish(const IconPen& p) {
    p.Dot(0.44f, 0.5f, 0.26f);
    p.Tri(0.70f, 0.5f, 0.94f, 0.28f, 0.94f, 0.72f);
    IconPen eye = p;
    eye.Color = glm::mix(p.Color, glm::vec3(0.0f), 0.6f);
    eye.Dot(0.32f, 0.44f, 0.05f);
}
void IconLeaf(const IconPen& p) {
    p.Dot(0.5f, 0.48f, 0.30f);
    p.Tri(0.5f, 0.05f, 0.80f, 0.55f, 0.20f, 0.55f);
    IconPen vein = p;
    vein.Color = glm::mix(p.Color, glm::vec3(0.0f), 0.35f);
    vein.Line(0.5f, 0.14f, 0.5f, 0.86f, 0.07f);
}
void IconOre(const IconPen& p) {
    p.Quad4(0.18f, 0.44f, 0.5f, 0.14f, 0.82f, 0.44f, 0.5f, 0.88f);
    IconPen inner = p;
    inner.Color = glm::mix(p.Color, glm::vec3(1.0f), 0.45f);
    inner.Quad4(0.36f, 0.44f, 0.5f, 0.30f, 0.64f, 0.44f, 0.5f, 0.60f);
}
void IconLantern(const IconPen& p) {
    p.Ring(0.5f, 0.22f, 0.14f, 0.07f);          // дужка
    p.Box(0.28f, 0.32f, 0.44f, 0.52f, 0.10f);   // корпус
    IconPen glow = p;
    glow.Color = glm::mix(p.Color, glm::vec3(1.0f), 0.6f);
    glow.Dot(0.5f, 0.58f, 0.13f);
}
void IconNet(const IconPen& p) {
    p.Ring(0.5f, 0.5f, 0.40f, 0.08f);
    p.Line(0.5f, 0.12f, 0.5f, 0.88f, 0.06f);
    p.Line(0.12f, 0.5f, 0.88f, 0.5f, 0.06f);
    p.Line(0.22f, 0.22f, 0.78f, 0.78f, 0.05f);
    p.Line(0.78f, 0.22f, 0.22f, 0.78f, 0.05f);
}
void IconAnchor(const IconPen& p) {
    p.Ring(0.5f, 0.20f, 0.11f, 0.07f);
    p.Line(0.5f, 0.28f, 0.5f, 0.86f, 0.09f);
    p.Line(0.26f, 0.44f, 0.74f, 0.44f, 0.08f);
    p.Line(0.16f, 0.62f, 0.30f, 0.84f, 0.09f);
    p.Line(0.84f, 0.62f, 0.70f, 0.84f, 0.09f);
}
void IconHook(const IconPen& p) { // багор
    p.Line(0.22f, 0.86f, 0.66f, 0.30f, 0.10f);
    p.Ring(0.72f, 0.26f, 0.16f, 0.09f);
}
void IconRod(const IconPen& p) { // удочка
    p.Line(0.14f, 0.84f, 0.78f, 0.18f, 0.08f);
    p.Line(0.78f, 0.18f, 0.62f, 0.70f, 0.04f);
    p.Dot(0.62f, 0.74f, 0.07f);
}
void IconSail(const IconPen& p) {
    p.Line(0.28f, 0.10f, 0.28f, 0.90f, 0.08f);
    p.Tri(0.34f, 0.14f, 0.34f, 0.78f, 0.86f, 0.62f);
}
void IconWall(const IconPen& p) {
    p.Box(0.10f, 0.20f, 0.80f, 0.20f, 0.04f);
    p.Box(0.10f, 0.44f, 0.36f, 0.20f, 0.04f);
    p.Box(0.52f, 0.44f, 0.38f, 0.20f, 0.04f);
    p.Box(0.10f, 0.68f, 0.80f, 0.20f, 0.04f);
}
void IconRail(const IconPen& p) {
    p.Line(0.08f, 0.30f, 0.92f, 0.30f, 0.09f);
    p.Line(0.24f, 0.30f, 0.24f, 0.82f, 0.08f);
    p.Line(0.50f, 0.30f, 0.50f, 0.82f, 0.08f);
    p.Line(0.76f, 0.30f, 0.76f, 0.82f, 0.08f);
}
void IconPurifier(const IconPen& p) {
    p.Box(0.20f, 0.30f, 0.60f, 0.56f, 0.10f);
    p.Line(0.34f, 0.30f, 0.34f, 0.12f, 0.07f);
    p.Line(0.66f, 0.30f, 0.66f, 0.12f, 0.07f);
    IconPen inner = p;
    inner.Color = glm::mix(p.Color, glm::vec3(1.0f), 0.5f);
    inner.Dot(0.5f, 0.60f, 0.12f);
}
void IconBoat(const IconPen& p) {
    p.Quad4(0.10f, 0.58f, 0.90f, 0.58f, 0.76f, 0.84f, 0.24f, 0.84f);
    p.Line(0.5f, 0.14f, 0.5f, 0.58f, 0.07f);
    p.Tri(0.54f, 0.18f, 0.54f, 0.50f, 0.84f, 0.42f);
}

} // namespace

void RegisterGameIcons() {
    struct Entry { const char* Name; sage::ui::IconDrawFn Draw; };
    static const Entry kIcons[] = {
        {"food", IconFood},         {"breath", IconBreath},   {"wave", IconWave},
        {"compass", IconCompass},   {"plank", IconPlank},     {"log", IconLog},
        {"crate", IconCrate},       {"barrel", IconBarrel},   {"rope", IconRope},
        {"cloth", IconCloth},       {"plastic", IconPlastic}, {"fish", IconFish},
        {"leaf", IconLeaf},         {"ore", IconOre},         {"lantern", IconLantern},
        {"net", IconNet},           {"anchor", IconAnchor},   {"hook", IconHook},
        {"rod", IconRod},           {"sail", IconSail},       {"wall", IconWall},
        {"rail", IconRail},         {"purifier", IconPurifier}, {"boat", IconBoat},
    };
    for (const Entry& e : kIcons) sage::ui::RegisterIcon(e.Name, e.Draw);
}

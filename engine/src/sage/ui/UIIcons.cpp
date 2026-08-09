#include "UIIcons.h"
#include "UIRenderer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace sage::ui {

namespace {

// Все иконки рисуются в НОРМАЛИЗОВАННЫХ координатах 0..1 внутри своего
// квадрата, а Pen переводит их в экранные. Так форма описывается один раз и
// одинаково выглядит в любом размере, а правки не превращаются в перенабор
// пиксельных координат.
struct Pen {
    UIRenderer& ui;
    float x, y, s;
    glm::vec3 c;
    float a;

    glm::vec2 P(float u, float v) const { return {x + u * s, y + v * s}; }
    float L(float k) const { return k * s; }

    void Dot(float u, float v, float r) const { ui.Circle(x + u * s, y + v * s, r * s, c, a); }
    void Ring(float u, float v, float r, float t) const {
        ui.Ring(x + u * s, y + v * s, r * s, std::max(t * s, 1.0f), c, a);
    }
    // Прямоугольник по левому-верхнему углу и размеру (всё в долях стороны).
    void Box(float u, float v, float w, float h, float round = 0.0f) const {
        ui.RoundedRect(x + u * s, y + v * s, w * s, h * s, c, a, round * s);
    }
    void Tri(float u0, float v0, float u1, float v1, float u2, float v2) const {
        ui.Triangle(P(u0, v0), P(u1, v1), P(u2, v2), c, a);
    }
    void Quad4(float u0, float v0, float u1, float v1, float u2, float v2,
               float u3, float v3) const {
        ui.Quad(P(u0, v0), P(u1, v1), P(u2, v2), P(u3, v3), c, a);
    }
    // Отрезок заданной толщины — четырёхугольник вдоль направления.
    void Line(float u0, float v0, float u1, float v1, float t) const {
        glm::vec2 p0 = P(u0, v0), p1 = P(u1, v1);
        glm::vec2 d = p1 - p0;
        float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < 0.0001f) return;
        glm::vec2 n{-d.y / len * L(t) * 0.5f, d.x / len * L(t) * 0.5f};
        ui.Quad(p0 + n, p1 + n, p1 - n, p0 - n, c, a);
    }
};

using IconFn = void (*)(const Pen&);

// --- Состояние игрока -------------------------------------------------------
void IconHeart(const Pen& p) {
    p.Dot(0.31f, 0.36f, 0.19f);
    p.Dot(0.69f, 0.36f, 0.19f);
    p.Tri(0.12f, 0.45f, 0.88f, 0.45f, 0.5f, 0.90f);
}
void IconDrop(const Pen& p) {
    p.Dot(0.5f, 0.63f, 0.27f);
    p.Tri(0.23f, 0.60f, 0.77f, 0.60f, 0.5f, 0.10f);
}
void IconFlame(const Pen& p) {
    p.Dot(0.5f, 0.66f, 0.25f);
    p.Tri(0.25f, 0.68f, 0.75f, 0.68f, 0.5f, 0.08f);
    // Светлое ядро: без него огонь читается просто каплей.
    Pen inner = p;
    inner.c = glm::mix(p.c, glm::vec3(1.0f), 0.55f);
    inner.Dot(0.5f, 0.70f, 0.13f);
}
void IconFood(const Pen& p) { // кусок с косточкой — «сытость»
    p.Dot(0.5f, 0.55f, 0.30f);
    Pen inner = p;
    inner.c = glm::mix(p.c, glm::vec3(0.0f), 0.35f);
    inner.Line(0.34f, 0.72f, 0.66f, 0.72f, 0.10f);
}
void IconBreath(const Pen& p) { // пузырьки воздуха
    p.Ring(0.42f, 0.55f, 0.26f, 0.09f);
    p.Ring(0.72f, 0.28f, 0.13f, 0.08f);
    p.Dot(0.84f, 0.62f, 0.06f);
}

// --- Мир и время ------------------------------------------------------------
void IconSun(const Pen& p) {
    p.Dot(0.5f, 0.5f, 0.22f);
    for (int i = 0; i < 8; ++i) {
        float ang = (float)i * 3.14159265f / 4.0f;
        float cx = 0.5f + std::cos(ang) * 0.36f;
        float cy = 0.5f + std::sin(ang) * 0.36f;
        p.Dot(cx, cy, 0.065f);
    }
}
void IconMoon(const Pen& p) {
    p.Dot(0.46f, 0.5f, 0.34f);
    // «Откусываем» серп фоном нельзя (фон произвольный) — рисуем полумесяц
    // из круга и вырезающего круга ТЕМ ЖЕ цветом с нулевой альфой не выйдет,
    // поэтому серп собран из двух дуг-колец.
    Pen ring = p;
    ring.Ring(0.62f, 0.44f, 0.30f, 0.30f);
}
void IconWave(const Pen& p) {
    p.Line(0.10f, 0.40f, 0.32f, 0.28f, 0.11f);
    p.Line(0.32f, 0.28f, 0.54f, 0.40f, 0.11f);
    p.Line(0.54f, 0.40f, 0.76f, 0.28f, 0.11f);
    p.Line(0.76f, 0.28f, 0.92f, 0.36f, 0.11f);
    p.Line(0.10f, 0.68f, 0.32f, 0.56f, 0.11f);
    p.Line(0.32f, 0.56f, 0.54f, 0.68f, 0.11f);
    p.Line(0.54f, 0.68f, 0.76f, 0.56f, 0.11f);
    p.Line(0.76f, 0.56f, 0.92f, 0.64f, 0.11f);
}
void IconClock(const Pen& p) {
    p.Ring(0.5f, 0.5f, 0.40f, 0.09f);
    p.Line(0.5f, 0.5f, 0.5f, 0.24f, 0.08f);
    p.Line(0.5f, 0.5f, 0.70f, 0.58f, 0.08f);
}
void IconCompass(const Pen& p) {
    p.Ring(0.5f, 0.5f, 0.40f, 0.08f);
    p.Tri(0.5f, 0.20f, 0.62f, 0.56f, 0.38f, 0.56f);
    p.Dot(0.5f, 0.62f, 0.07f);
}

// --- Материалы и предметы ---------------------------------------------------
void IconPlank(const Pen& p) {
    p.Box(0.10f, 0.30f, 0.80f, 0.18f, 0.05f);
    p.Box(0.10f, 0.54f, 0.80f, 0.18f, 0.05f);
}
void IconLog(const Pen& p) {
    p.Box(0.08f, 0.34f, 0.84f, 0.32f, 0.14f);
    Pen inner = p;
    inner.c = glm::mix(p.c, glm::vec3(0.0f), 0.35f);
    inner.Ring(0.80f, 0.50f, 0.14f, 0.07f);
}
void IconCrate(const Pen& p) {
    p.Box(0.12f, 0.18f, 0.76f, 0.64f, 0.07f);
    Pen inner = p;
    inner.c = glm::mix(p.c, glm::vec3(0.0f), 0.4f);
    inner.Line(0.12f, 0.18f, 0.88f, 0.82f, 0.09f);
    inner.Line(0.88f, 0.18f, 0.12f, 0.82f, 0.09f);
}
void IconBarrel(const Pen& p) {
    p.Box(0.20f, 0.14f, 0.60f, 0.72f, 0.22f);
    Pen inner = p;
    inner.c = glm::mix(p.c, glm::vec3(0.0f), 0.4f);
    inner.Box(0.18f, 0.32f, 0.64f, 0.08f, 0.03f);
    inner.Box(0.18f, 0.60f, 0.64f, 0.08f, 0.03f);
}
void IconRope(const Pen& p) {
    p.Ring(0.5f, 0.5f, 0.36f, 0.10f);
    p.Ring(0.5f, 0.5f, 0.19f, 0.09f);
}
void IconCloth(const Pen& p) {
    p.Quad4(0.14f, 0.22f, 0.86f, 0.14f, 0.86f, 0.72f, 0.14f, 0.84f);
    Pen inner = p;
    inner.c = glm::mix(p.c, glm::vec3(0.0f), 0.3f);
    inner.Line(0.14f, 0.53f, 0.86f, 0.43f, 0.06f);
}
void IconPlastic(const Pen& p) { // бутылка
    p.Box(0.36f, 0.10f, 0.28f, 0.16f, 0.05f);
    p.Box(0.26f, 0.26f, 0.48f, 0.62f, 0.14f);
}
void IconFish(const Pen& p) {
    p.Dot(0.44f, 0.5f, 0.26f);
    p.Tri(0.70f, 0.5f, 0.94f, 0.28f, 0.94f, 0.72f);
    Pen eye = p;
    eye.c = glm::mix(p.c, glm::vec3(0.0f), 0.6f);
    eye.Dot(0.32f, 0.44f, 0.05f);
}
void IconLeaf(const Pen& p) {
    p.Dot(0.5f, 0.48f, 0.30f);
    p.Tri(0.5f, 0.05f, 0.80f, 0.55f, 0.20f, 0.55f);
    Pen vein = p;
    vein.c = glm::mix(p.c, glm::vec3(0.0f), 0.35f);
    vein.Line(0.5f, 0.14f, 0.5f, 0.86f, 0.07f);
}
void IconOre(const Pen& p) {
    p.Quad4(0.18f, 0.44f, 0.5f, 0.14f, 0.82f, 0.44f, 0.5f, 0.88f);
    Pen inner = p;
    inner.c = glm::mix(p.c, glm::vec3(1.0f), 0.45f);
    inner.Quad4(0.36f, 0.44f, 0.5f, 0.30f, 0.64f, 0.44f, 0.5f, 0.60f);
}

// --- Постройки и инструменты ------------------------------------------------
void IconLantern(const Pen& p) {
    p.Ring(0.5f, 0.22f, 0.14f, 0.07f);          // дужка
    p.Box(0.28f, 0.32f, 0.44f, 0.52f, 0.10f);   // корпус
    Pen glow = p;
    glow.c = glm::mix(p.c, glm::vec3(1.0f), 0.6f);
    glow.Dot(0.5f, 0.58f, 0.13f);
}
void IconNet(const Pen& p) {
    p.Ring(0.5f, 0.5f, 0.40f, 0.08f);
    p.Line(0.5f, 0.12f, 0.5f, 0.88f, 0.06f);
    p.Line(0.12f, 0.5f, 0.88f, 0.5f, 0.06f);
    p.Line(0.22f, 0.22f, 0.78f, 0.78f, 0.05f);
    p.Line(0.78f, 0.22f, 0.22f, 0.78f, 0.05f);
}
void IconAnchor(const Pen& p) {
    p.Ring(0.5f, 0.20f, 0.11f, 0.07f);
    p.Line(0.5f, 0.28f, 0.5f, 0.86f, 0.09f);
    p.Line(0.26f, 0.44f, 0.74f, 0.44f, 0.08f);
    p.Line(0.16f, 0.62f, 0.30f, 0.84f, 0.09f);
    p.Line(0.84f, 0.62f, 0.70f, 0.84f, 0.09f);
}
void IconHook(const Pen& p) { // багор
    p.Line(0.22f, 0.86f, 0.66f, 0.30f, 0.10f);
    p.Ring(0.72f, 0.26f, 0.16f, 0.09f);
}
void IconRod(const Pen& p) { // удочка
    p.Line(0.14f, 0.84f, 0.78f, 0.18f, 0.08f);
    p.Line(0.78f, 0.18f, 0.62f, 0.70f, 0.04f);
    p.Dot(0.62f, 0.74f, 0.07f);
}
void IconSail(const Pen& p) {
    p.Line(0.28f, 0.10f, 0.28f, 0.90f, 0.08f);
    p.Tri(0.34f, 0.14f, 0.34f, 0.78f, 0.86f, 0.62f);
}
void IconWall(const Pen& p) {
    p.Box(0.10f, 0.20f, 0.80f, 0.20f, 0.04f);
    p.Box(0.10f, 0.44f, 0.36f, 0.20f, 0.04f);
    p.Box(0.52f, 0.44f, 0.38f, 0.20f, 0.04f);
    p.Box(0.10f, 0.68f, 0.80f, 0.20f, 0.04f);
}
void IconRail(const Pen& p) {
    p.Line(0.08f, 0.30f, 0.92f, 0.30f, 0.09f);
    p.Line(0.24f, 0.30f, 0.24f, 0.82f, 0.08f);
    p.Line(0.50f, 0.30f, 0.50f, 0.82f, 0.08f);
    p.Line(0.76f, 0.30f, 0.76f, 0.82f, 0.08f);
}
void IconPurifier(const Pen& p) {
    p.Box(0.20f, 0.30f, 0.60f, 0.56f, 0.10f);
    p.Line(0.34f, 0.30f, 0.34f, 0.12f, 0.07f);
    p.Line(0.66f, 0.30f, 0.66f, 0.12f, 0.07f);
    Pen inner = p;
    inner.c = glm::mix(p.c, glm::vec3(1.0f), 0.5f);
    inner.Dot(0.5f, 0.60f, 0.12f);
}
void IconBoat(const Pen& p) {
    p.Quad4(0.10f, 0.58f, 0.90f, 0.58f, 0.76f, 0.84f, 0.24f, 0.84f);
    p.Line(0.5f, 0.14f, 0.5f, 0.58f, 0.07f);
    p.Tri(0.54f, 0.18f, 0.54f, 0.50f, 0.84f, 0.42f);
}

// --- Служебные --------------------------------------------------------------
void IconPlus(const Pen& p) {
    p.Line(0.5f, 0.16f, 0.5f, 0.84f, 0.15f);
    p.Line(0.16f, 0.5f, 0.84f, 0.5f, 0.15f);
}
void IconMinus(const Pen& p) { p.Line(0.16f, 0.5f, 0.84f, 0.5f, 0.15f); }
void IconCheck(const Pen& p) {
    p.Line(0.16f, 0.52f, 0.42f, 0.76f, 0.14f);
    p.Line(0.42f, 0.76f, 0.86f, 0.22f, 0.14f);
}
void IconCross(const Pen& p) {
    p.Line(0.20f, 0.20f, 0.80f, 0.80f, 0.14f);
    p.Line(0.80f, 0.20f, 0.20f, 0.80f, 0.14f);
}
void IconWarn(const Pen& p) {
    p.Tri(0.5f, 0.10f, 0.94f, 0.86f, 0.06f, 0.86f);
    Pen inner = p;
    inner.c = glm::mix(p.c, glm::vec3(0.0f), 0.65f);
    inner.Line(0.5f, 0.38f, 0.5f, 0.62f, 0.10f);
    inner.Dot(0.5f, 0.74f, 0.055f);
}
void IconBag(const Pen& p) {
    p.Box(0.14f, 0.34f, 0.72f, 0.54f, 0.12f);
    p.Ring(0.5f, 0.34f, 0.19f, 0.08f);
}

// --- Меню -------------------------------------------------------------------
//
// Значки экранов, а не мира: продолжить, сохранить, настроить, выйти, собрать.
// Без них меню игры собирается из одних надписей — а «Продолжить» и «Выйти»,
// набранные одинаковым текстом в одинаковых прямоугольниках, различаются
// только чтением, и промахнуться по ним легко именно там, где цена промаха
// наибольшая (выход из игры стоит рядом с продолжением).
void IconPlay(const Pen& p) { p.Tri(0.28f, 0.16f, 0.28f, 0.84f, 0.84f, 0.50f); }
void IconPause(const Pen& p) {
    p.Box(0.26f, 0.18f, 0.16f, 0.64f, 0.04f);
    p.Box(0.58f, 0.18f, 0.16f, 0.64f, 0.04f);
}
void IconGear(const Pen& p) {
    // Зубцы — восемь коротких отрезков по кругу, вокруг кольца: рисовать
    // настоящую шестерню многоугольником здесь нечем, а на двадцати пикселях
    // разницы всё равно не видно.
    for (int i = 0; i < 8; ++i) {
        const float ang = (float)i * 3.14159265f / 4.0f;
        const float cx = std::cos(ang), cy = std::sin(ang);
        p.Line(0.5f + cx * 0.26f, 0.5f + cy * 0.26f, 0.5f + cx * 0.46f, 0.5f + cy * 0.46f, 0.16f);
    }
    p.Ring(0.5f, 0.5f, 0.28f, 0.13f);
}
void IconSave(const Pen& p) { // дискета: узнаётся даже теми, кто их не застал
    p.Box(0.14f, 0.14f, 0.72f, 0.72f, 0.08f);
    Pen inner = p;
    inner.c = glm::mix(p.c, glm::vec3(0.0f), 0.55f);
    inner.Box(0.30f, 0.16f, 0.40f, 0.24f, 0.03f);  // шторка
    inner.Box(0.24f, 0.52f, 0.52f, 0.32f, 0.03f);  // наклейка
}
void IconExit(const Pen& p) { // дверь со стрелкой наружу
    p.Box(0.14f, 0.12f, 0.38f, 0.76f, 0.06f);
    p.Line(0.56f, 0.50f, 0.88f, 0.50f, 0.10f);
    p.Tri(0.72f, 0.30f, 0.72f, 0.70f, 0.94f, 0.50f);
}
void IconHammer(const Pen& p) { // крафт
    p.Line(0.30f, 0.86f, 0.66f, 0.36f, 0.11f);
    p.Quad4(0.44f, 0.22f, 0.72f, 0.06f, 0.90f, 0.32f, 0.62f, 0.48f);
}
void IconLamp(const Pen& p) { // фонарик в руке: конус света
    p.Box(0.12f, 0.38f, 0.30f, 0.24f, 0.06f);
    p.Tri(0.44f, 0.18f, 0.44f, 0.82f, 0.88f, 0.50f);
}

const std::unordered_map<std::string, IconFn>& Table() {
    static const std::unordered_map<std::string, IconFn> kTable = {
        {"heart", IconHeart}, {"drop", IconDrop}, {"flame", IconFlame},
        {"food", IconFood},   {"breath", IconBreath},
        {"sun", IconSun},     {"moon", IconMoon}, {"wave", IconWave},
        {"clock", IconClock}, {"compass", IconCompass},
        {"plank", IconPlank}, {"log", IconLog},   {"crate", IconCrate},
        {"barrel", IconBarrel}, {"rope", IconRope}, {"cloth", IconCloth},
        {"plastic", IconPlastic}, {"fish", IconFish}, {"leaf", IconLeaf},
        {"ore", IconOre},
        {"lantern", IconLantern}, {"net", IconNet}, {"anchor", IconAnchor},
        {"hook", IconHook},   {"rod", IconRod},   {"sail", IconSail},
        {"wall", IconWall},   {"rail", IconRail}, {"purifier", IconPurifier},
        {"boat", IconBoat},
        {"plus", IconPlus},   {"minus", IconMinus}, {"check", IconCheck},
        {"cross", IconCross}, {"warn", IconWarn}, {"bag", IconBag},
        {"play", IconPlay},   {"pause", IconPause}, {"gear", IconGear},
        {"save", IconSave},   {"exit", IconExit}, {"hammer", IconHammer},
        {"lamp", IconLamp},
    };
    return kTable;
}

} // namespace

bool HasIcon(const std::string& name) {
    return Table().find(name) != Table().end();
}

const std::vector<std::string>& IconNames() {
    static const std::vector<std::string> kNames = [] {
        std::vector<std::string> v;
        v.reserve(Table().size());
        for (const auto& kv : Table()) v.push_back(kv.first);
        std::sort(v.begin(), v.end());
        return v;
    }();
    return kNames;
}

bool DrawIcon(UIRenderer& ui, const std::string& name, float x, float y, float size,
              glm::vec3 color, float alpha) {
    if (size <= 0.0f || alpha <= 0.0f) return HasIcon(name);
    Pen pen{ui, x, y, size, color, alpha};
    auto it = Table().find(name);
    if (it == Table().end()) {
        // Заглушка «нет такой иконки»: рамка с перечёркиванием. Видно сразу и
        // не рушит вёрстку — место под иконку остаётся тем же.
        ui.RoundedRectOutline(x, y, size, size, size * 0.15f, std::max(size * 0.06f, 1.0f),
                              color, alpha * 0.7f);
        pen.Line(0.22f, 0.22f, 0.78f, 0.78f, 0.08f);
        return false;
    }
    it->second(pen);
    return true;
}

} // namespace sage::ui

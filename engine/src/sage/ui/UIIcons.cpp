#include "UIIcons.h"
#include "UIRenderer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace sage::ui {

// --- Перо: доли стороны -> пиксели ------------------------------------------
//
// Определения здесь, а не в заголовке: рисование опирается на весь UIRenderer,
// и тащить его в каждый файл, которому нужно лишь имя иконки, незачем.

glm::vec2 IconPen::P(float u, float v) const { return {X + u * Size, Y + v * Size}; }
float IconPen::L(float k) const { return k * Size; }

void IconPen::Dot(float u, float v, float r) const {
    Ui.Circle(X + u * Size, Y + v * Size, r * Size, Color, Alpha);
}
void IconPen::Ring(float u, float v, float r, float t) const {
    Ui.Ring(X + u * Size, Y + v * Size, r * Size, std::max(t * Size, 1.0f), Color, Alpha);
}
void IconPen::Box(float u, float v, float w, float h, float round) const {
    Ui.RoundedRect(X + u * Size, Y + v * Size, w * Size, h * Size, Color, Alpha, round * Size);
}
void IconPen::Tri(float u0, float v0, float u1, float v1, float u2, float v2) const {
    Ui.Triangle(P(u0, v0), P(u1, v1), P(u2, v2), Color, Alpha);
}
void IconPen::Quad4(float u0, float v0, float u1, float v1, float u2, float v2, float u3,
                    float v3) const {
    Ui.Quad(P(u0, v0), P(u1, v1), P(u2, v2), P(u3, v3), Color, Alpha);
}
void IconPen::Line(float u0, float v0, float u1, float v1, float t) const {
    glm::vec2 p0 = P(u0, v0), p1 = P(u1, v1);
    glm::vec2 d = p1 - p0;
    float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 0.0001f) return;
    glm::vec2 n{-d.y / len * L(t) * 0.5f, d.x / len * L(t) * 0.5f};
    Ui.Quad(p0 + n, p1 + n, p1 - n, p0 - n, Color, Alpha);
}

namespace {

void IconHeart(const IconPen& p) {
    p.Dot(0.31f, 0.36f, 0.19f);
    p.Dot(0.69f, 0.36f, 0.19f);
    p.Tri(0.12f, 0.45f, 0.88f, 0.45f, 0.5f, 0.90f);
}
void IconDrop(const IconPen& p) {
    p.Dot(0.5f, 0.63f, 0.27f);
    p.Tri(0.23f, 0.60f, 0.77f, 0.60f, 0.5f, 0.10f);
}
void IconFlame(const IconPen& p) {
    p.Dot(0.5f, 0.66f, 0.25f);
    p.Tri(0.25f, 0.68f, 0.75f, 0.68f, 0.5f, 0.08f);
    // Светлое ядро: без него огонь читается просто каплей.
    IconPen inner = p;
    inner.Color = glm::mix(p.Color, glm::vec3(1.0f), 0.55f);
    inner.Dot(0.5f, 0.70f, 0.13f);
}
void IconSun(const IconPen& p) {
    p.Dot(0.5f, 0.5f, 0.22f);
    for (int i = 0; i < 8; ++i) {
        float ang = (float)i * 3.14159265f / 4.0f;
        float cx = 0.5f + std::cos(ang) * 0.36f;
        float cy = 0.5f + std::sin(ang) * 0.36f;
        p.Dot(cx, cy, 0.065f);
    }
}
void IconMoon(const IconPen& p) {
    p.Dot(0.46f, 0.5f, 0.34f);
    // «Откусываем» серп фоном нельзя (фон произвольный) — рисуем полумесяц
    // из круга и вырезающего круга ТЕМ ЖЕ цветом с нулевой альфой не выйдет,
    // поэтому серп собран из двух дуг-колец.
    IconPen ring = p;
    ring.Ring(0.62f, 0.44f, 0.30f, 0.30f);
}
void IconClock(const IconPen& p) {
    p.Ring(0.5f, 0.5f, 0.40f, 0.09f);
    p.Line(0.5f, 0.5f, 0.5f, 0.24f, 0.08f);
    p.Line(0.5f, 0.5f, 0.70f, 0.58f, 0.08f);
}
void IconPlus(const IconPen& p) {
    p.Line(0.5f, 0.16f, 0.5f, 0.84f, 0.15f);
    p.Line(0.16f, 0.5f, 0.84f, 0.5f, 0.15f);
}
void IconMinus(const IconPen& p) { p.Line(0.16f, 0.5f, 0.84f, 0.5f, 0.15f); }
void IconCheck(const IconPen& p) {
    p.Line(0.16f, 0.52f, 0.42f, 0.76f, 0.14f);
    p.Line(0.42f, 0.76f, 0.86f, 0.22f, 0.14f);
}
void IconCross(const IconPen& p) {
    p.Line(0.20f, 0.20f, 0.80f, 0.80f, 0.14f);
    p.Line(0.80f, 0.20f, 0.20f, 0.80f, 0.14f);
}
void IconWarn(const IconPen& p) {
    p.Tri(0.5f, 0.10f, 0.94f, 0.86f, 0.06f, 0.86f);
    IconPen inner = p;
    inner.Color = glm::mix(p.Color, glm::vec3(0.0f), 0.65f);
    inner.Line(0.5f, 0.38f, 0.5f, 0.62f, 0.10f);
    inner.Dot(0.5f, 0.74f, 0.055f);
}
void IconBag(const IconPen& p) {
    p.Box(0.14f, 0.34f, 0.72f, 0.54f, 0.12f);
    p.Ring(0.5f, 0.34f, 0.19f, 0.08f);
}
void IconPlay(const IconPen& p) { p.Tri(0.28f, 0.16f, 0.28f, 0.84f, 0.84f, 0.50f); }
void IconPause(const IconPen& p) {
    p.Box(0.26f, 0.18f, 0.16f, 0.64f, 0.04f);
    p.Box(0.58f, 0.18f, 0.16f, 0.64f, 0.04f);
}
void IconGear(const IconPen& p) {
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
void IconSave(const IconPen& p) { // дискета: узнаётся даже теми, кто их не застал
    p.Box(0.14f, 0.14f, 0.72f, 0.72f, 0.08f);
    IconPen inner = p;
    inner.Color = glm::mix(p.Color, glm::vec3(0.0f), 0.55f);
    inner.Box(0.30f, 0.16f, 0.40f, 0.24f, 0.03f);  // шторка
    inner.Box(0.24f, 0.52f, 0.52f, 0.32f, 0.03f);  // наклейка
}
void IconExit(const IconPen& p) { // дверь со стрелкой наружу
    p.Box(0.14f, 0.12f, 0.38f, 0.76f, 0.06f);
    p.Line(0.56f, 0.50f, 0.88f, 0.50f, 0.10f);
    p.Tri(0.72f, 0.30f, 0.72f, 0.70f, 0.94f, 0.50f);
}
void IconHammer(const IconPen& p) { // крафт
    p.Line(0.30f, 0.86f, 0.66f, 0.36f, 0.11f);
    p.Quad4(0.44f, 0.22f, 0.72f, 0.06f, 0.90f, 0.32f, 0.62f, 0.48f);
}
void IconLamp(const IconPen& p) { // фонарик в руке: конус света
    p.Box(0.12f, 0.38f, 0.30f, 0.24f, 0.06f);
    p.Tri(0.44f, 0.18f, 0.44f, 0.82f, 0.88f, 0.50f);
}

// --- Набор движка -----------------------------------------------------------
//
// Только НЕЙТРАЛЬНОЕ: управление, разметка, состояние. Предметы конкретного
// мира (доска, бочка, парус, водоочиститель) приносит игра через RegisterIcon —
// см. заголовок и games/testgame/src/GameIcons.cpp.
//
// Таблица теперь ЖИВАЯ, а не константа: в неё дописывает игра. Отсюда и версия
// — по ней пересобирается список имён (IconNames), который редактор показывает
// в инспекторе: зарегистрированная игрой иконка обязана быть в нём видна.
std::unordered_map<std::string, IconDrawFn>& Table() {
    static std::unordered_map<std::string, IconDrawFn> kTable = {
        {"heart", IconHeart}, {"drop", IconDrop},   {"flame", IconFlame},
        {"sun", IconSun},     {"moon", IconMoon},   {"clock", IconClock},
        {"plus", IconPlus},   {"minus", IconMinus}, {"check", IconCheck},
        {"cross", IconCross}, {"warn", IconWarn},   {"bag", IconBag},
        {"play", IconPlay},   {"pause", IconPause}, {"gear", IconGear},
        {"save", IconSave},   {"exit", IconExit},   {"hammer", IconHammer},
        {"lamp", IconLamp},
    };
    return kTable;
}

unsigned& TableVersion() {
    static unsigned v = 0;
    return v;
}

} // namespace

void RegisterIcon(const std::string& name, IconDrawFn draw) {
    if (name.empty() || !draw) return;
    Table()[name] = draw;
    ++TableVersion();
}

bool HasIcon(const std::string& name) {
    return Table().find(name) != Table().end();
}

const std::vector<std::string>& IconNames() {
    static std::vector<std::string> names;
    static unsigned built = (unsigned)-1;
    if (built != TableVersion()) {
        names.clear();
        names.reserve(Table().size());
        for (const auto& kv : Table()) names.push_back(kv.first);
        std::sort(names.begin(), names.end());
        built = TableVersion();
    }
    return names;
}

bool DrawIcon(UIRenderer& ui, const std::string& name, float x, float y, float size,
              glm::vec3 color, float alpha) {
    if (size <= 0.0f || alpha <= 0.0f) return HasIcon(name);
    IconPen pen{ui, x, y, size, color, alpha};
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

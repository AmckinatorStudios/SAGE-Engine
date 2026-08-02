#include "EditorIcons.h"

#include <cmath>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h"

namespace EditorIcons {

namespace {

ImU32 Col(const glm::vec3& c, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, a));
}

// Все иконки рисуются в квадрате (o.x, o.y, s, s). Координаты внутри задаются
// долями стороны — тогда одна и та же формула годится и для 12, и для 32
// пикселей, и не приходится держать по набору констант на размер.
struct Pen {
    ImDrawList* dl;
    ImVec2 o;
    float s;
    ImU32 c;

    ImVec2 P(float x, float y) const { return ImVec2(o.x + x * s, o.y + y * s); }
    void Line(float x0, float y0, float x1, float y1, float w = 0.09f) const {
        dl->AddLine(P(x0, y0), P(x1, y1), c, w * s);
    }
    void Rect(float x0, float y0, float x1, float y1, float w = 0.09f, float r = 0.0f) const {
        dl->AddRect(P(x0, y0), P(x1, y1), c, r * s, 0, w * s);
    }
    void FillRect(float x0, float y0, float x1, float y1, float r = 0.0f) const {
        dl->AddRectFilled(P(x0, y0), P(x1, y1), c, r * s);
    }
    void Circle(float cx, float cy, float rad, float w = 0.09f, int seg = 16) const {
        dl->AddCircle(P(cx, cy), rad * s, c, seg, w * s);
    }
    void Disc(float cx, float cy, float rad, int seg = 16) const {
        dl->AddCircleFilled(P(cx, cy), rad * s, c, seg);
    }
    void Tri(float x0, float y0, float x1, float y1, float x2, float y2) const {
        dl->AddTriangleFilled(P(x0, y0), P(x1, y1), P(x2, y2), c);
    }
    // Дуга: угол в оборотах (0..1), чтобы не сыпать pi по коду.
    void Arc(float cx, float cy, float rad, float from, float to, float w = 0.09f) const {
        dl->PathArcTo(P(cx, cy), rad * s, from * 6.2831853f, to * 6.2831853f, 20);
        dl->PathStroke(c, 0, w * s);
    }
};

// Единственное место, где живёт форма каждой иконки. Возвращает false для
// незнакомого имени — вызывающий рисует заглушку.
bool Shape(const Pen& p, const char* n) {
    auto is = [&](const char* x) { return std::strcmp(n, x) == 0; };

    if (is("play"))     { p.Tri(0.28f, 0.18f, 0.28f, 0.82f, 0.80f, 0.50f); return true; }
    if (is("pause"))    { p.FillRect(0.28f, 0.20f, 0.44f, 0.80f); p.FillRect(0.56f, 0.20f, 0.72f, 0.80f); return true; }
    if (is("stop"))     { p.FillRect(0.25f, 0.25f, 0.75f, 0.75f, 0.08f); return true; }
    if (is("step"))     { p.Tri(0.24f, 0.20f, 0.24f, 0.80f, 0.62f, 0.50f); p.FillRect(0.66f, 0.20f, 0.78f, 0.80f); return true; }

    // Гизмо: стрелка (перенос), дуга со стрелкой (поворот), квадрат с ручкой.
    if (is("move")) {
        p.Line(0.50f, 0.16f, 0.50f, 0.84f);
        p.Line(0.16f, 0.50f, 0.84f, 0.50f);
        p.Tri(0.50f, 0.10f, 0.40f, 0.26f, 0.60f, 0.26f);
        p.Tri(0.90f, 0.50f, 0.74f, 0.40f, 0.74f, 0.60f);
        return true;
    }
    // Универсальное гизмо: в одном значке всё три операции сразу — крест
    // переноса, кольцо поворота и уголок масштаба.
    if (is("universal")) {
        p.Line(0.50f, 0.20f, 0.50f, 0.80f, 0.07f);
        p.Line(0.20f, 0.50f, 0.80f, 0.50f, 0.07f);
        p.Circle(0.50f, 0.50f, 0.22f, 0.06f);
        p.Rect(0.62f, 0.62f, 0.86f, 0.86f, 0.07f);
        return true;
    }
    // Рамка: прямоугольник с ручками по углам — то, за что её и тянут.
    if (is("rect")) {
        p.Rect(0.24f, 0.28f, 0.76f, 0.72f, 0.07f);
        p.FillRect(0.16f, 0.20f, 0.32f, 0.36f);
        p.FillRect(0.68f, 0.20f, 0.84f, 0.36f);
        p.FillRect(0.16f, 0.64f, 0.32f, 0.80f);
        p.FillRect(0.68f, 0.64f, 0.84f, 0.80f);
        return true;
    }
    // Выравнивание: три бруска, прижатых к одной линии.
    if (is("align")) {
        p.Line(0.18f, 0.14f, 0.18f, 0.86f, 0.08f);
        p.FillRect(0.28f, 0.20f, 0.84f, 0.34f);
        p.FillRect(0.28f, 0.43f, 0.64f, 0.57f);
        p.FillRect(0.28f, 0.66f, 0.78f, 0.80f);
        return true;
    }
    // Посадить на поверхность: стрелка вниз, упирающаяся в опору.
    if (is("drop")) {
        p.Line(0.50f, 0.12f, 0.50f, 0.60f, 0.09f);
        p.Tri(0.50f, 0.74f, 0.32f, 0.52f, 0.68f, 0.52f);
        p.FillRect(0.16f, 0.82f, 0.84f, 0.90f);
        return true;
    }
    if (is("rotate")) {
        p.Arc(0.50f, 0.52f, 0.30f, 0.60f, 1.15f);
        p.Tri(0.78f, 0.30f, 0.62f, 0.34f, 0.74f, 0.48f);
        return true;
    }
    if (is("scale")) {
        p.Rect(0.18f, 0.52f, 0.48f, 0.82f, 0.08f);
        p.Line(0.52f, 0.48f, 0.82f, 0.18f);
        p.Tri(0.86f, 0.14f, 0.62f, 0.18f, 0.82f, 0.38f);
        return true;
    }

    if (is("grid")) {
        for (int i = 1; i <= 2; ++i) {
            const float t = 0.20f + 0.20f * (float)i;
            p.Line(t, 0.18f, t, 0.82f, 0.06f);
            p.Line(0.18f, t, 0.82f, t, 0.06f);
        }
        p.Rect(0.18f, 0.18f, 0.82f, 0.82f, 0.07f);
        return true;
    }
    if (is("wire")) {
        p.Rect(0.20f, 0.20f, 0.68f, 0.68f, 0.07f);
        p.Rect(0.32f, 0.32f, 0.80f, 0.80f, 0.07f);
        p.Line(0.20f, 0.20f, 0.32f, 0.32f, 0.06f);
        p.Line(0.68f, 0.68f, 0.80f, 0.80f, 0.06f);
        return true;
    }

    // Типы сущностей — по ним читается иерархия.
    if (is("cube")) {
        p.Line(0.50f, 0.14f, 0.84f, 0.32f); p.Line(0.84f, 0.32f, 0.84f, 0.68f);
        p.Line(0.84f, 0.68f, 0.50f, 0.86f); p.Line(0.50f, 0.86f, 0.16f, 0.68f);
        p.Line(0.16f, 0.68f, 0.16f, 0.32f); p.Line(0.16f, 0.32f, 0.50f, 0.14f);
        p.Line(0.50f, 0.50f, 0.50f, 0.86f, 0.06f);
        p.Line(0.50f, 0.50f, 0.16f, 0.32f, 0.06f);
        p.Line(0.50f, 0.50f, 0.84f, 0.32f, 0.06f);
        return true;
    }
    if (is("sphere")) { p.Circle(0.50f, 0.50f, 0.34f); p.Arc(0.50f, 0.50f, 0.34f, 0.0f, 0.5f, 0.05f); return true; }
    if (is("light"))  { p.Disc(0.50f, 0.44f, 0.17f); for (int i = 0; i < 8; ++i) { const float a = (float)i / 8.0f * 6.2831853f; p.Line(0.50f + std::cos(a) * 0.24f, 0.44f + std::sin(a) * 0.24f, 0.50f + std::cos(a) * 0.36f, 0.44f + std::sin(a) * 0.36f, 0.06f); } return true; }
    if (is("sun"))    { p.Circle(0.50f, 0.50f, 0.20f); for (int i = 0; i < 8; ++i) { const float a = (float)i / 8.0f * 6.2831853f; p.Line(0.50f + std::cos(a) * 0.28f, 0.50f + std::sin(a) * 0.28f, 0.50f + std::cos(a) * 0.40f, 0.50f + std::sin(a) * 0.40f, 0.06f); } return true; }
    if (is("camera")) { p.Rect(0.14f, 0.32f, 0.62f, 0.70f, 0.08f, 0.06f); p.Tri(0.66f, 0.51f, 0.86f, 0.34f, 0.86f, 0.68f); return true; }
    if (is("script")) { p.Rect(0.22f, 0.14f, 0.78f, 0.86f, 0.08f, 0.06f); p.Line(0.34f, 0.34f, 0.66f, 0.34f, 0.06f); p.Line(0.34f, 0.50f, 0.66f, 0.50f, 0.06f); p.Line(0.34f, 0.66f, 0.56f, 0.66f, 0.06f); return true; }
    if (is("particles")) { p.Disc(0.34f, 0.66f, 0.11f); p.Disc(0.62f, 0.44f, 0.08f); p.Disc(0.46f, 0.26f, 0.06f); p.Disc(0.74f, 0.72f, 0.05f); return true; }
    if (is("anim"))   { p.Circle(0.30f, 0.66f, 0.11f); p.Circle(0.70f, 0.34f, 0.11f); p.Line(0.38f, 0.58f, 0.62f, 0.42f); return true; }
    if (is("ik"))     { p.Disc(0.22f, 0.22f, 0.08f); p.Disc(0.50f, 0.58f, 0.08f); p.Disc(0.80f, 0.30f, 0.08f); p.Line(0.22f, 0.22f, 0.50f, 0.58f, 0.07f); p.Line(0.50f, 0.58f, 0.80f, 0.30f, 0.07f); return true; }
    if (is("probe"))  { p.Circle(0.50f, 0.50f, 0.24f); p.Rect(0.14f, 0.14f, 0.86f, 0.86f, 0.05f, 0.04f); p.Arc(0.42f, 0.42f, 0.10f, 0.55f, 0.85f, 0.06f); return true; }
    if (is("physics")) { p.Circle(0.50f, 0.50f, 0.30f); p.Line(0.50f, 0.20f, 0.50f, 0.80f, 0.05f); p.Line(0.20f, 0.50f, 0.80f, 0.50f, 0.05f); return true; }

    // Ассеты.
    if (is("folder")) { p.FillRect(0.12f, 0.28f, 0.46f, 0.38f, 0.03f); p.Rect(0.12f, 0.32f, 0.88f, 0.76f, 0.08f, 0.05f); return true; }
    if (is("file"))   { p.Rect(0.24f, 0.14f, 0.76f, 0.86f, 0.08f, 0.05f); p.Line(0.60f, 0.14f, 0.76f, 0.30f, 0.07f); return true; }
    if (is("scene"))  { p.Rect(0.14f, 0.22f, 0.86f, 0.78f, 0.08f, 0.05f); p.Tri(0.24f, 0.68f, 0.46f, 0.38f, 0.62f, 0.68f); p.Disc(0.68f, 0.36f, 0.07f); return true; }
    if (is("material")) { p.Circle(0.50f, 0.50f, 0.32f); p.Disc(0.40f, 0.40f, 0.10f); return true; }
    if (is("project")) { p.FillRect(0.12f, 0.24f, 0.44f, 0.34f, 0.03f); p.Rect(0.12f, 0.30f, 0.88f, 0.78f, 0.08f, 0.05f); p.Disc(0.50f, 0.54f, 0.09f); return true; }
    if (is("prefab")) { p.Rect(0.16f, 0.16f, 0.60f, 0.60f, 0.06f, 0.05f); p.Rect(0.40f, 0.40f, 0.84f, 0.84f, 0.06f, 0.05f); return true; }
    if (is("texture")) { p.Rect(0.14f, 0.20f, 0.86f, 0.80f, 0.08f, 0.05f); p.Tri(0.22f, 0.72f, 0.42f, 0.42f, 0.60f, 0.72f); p.Disc(0.66f, 0.36f, 0.07f); p.Line(0.14f, 0.72f, 0.86f, 0.72f, 0.05f); return true; }
    if (is("shader")) { p.Circle(0.50f, 0.50f, 0.30f); p.Arc(0.50f, 0.50f, 0.30f, 0.75f, 1.25f, 0.14f); return true; }
    if (is("audio")) { p.Tri(0.20f, 0.50f, 0.46f, 0.26f, 0.46f, 0.74f); p.Rect(0.20f, 0.40f, 0.34f, 0.60f, 0.02f, 0.05f); p.Arc(0.50f, 0.50f, 0.18f, -0.20f, 0.20f, 0.06f); p.Arc(0.50f, 0.50f, 0.30f, -0.18f, 0.18f, 0.06f); return true; }

    // Навигация и действия файлового диалога.
    if (is("up")) { p.Line(0.50f, 0.80f, 0.50f, 0.24f, 0.08f); p.Line(0.50f, 0.24f, 0.28f, 0.46f, 0.08f); p.Line(0.50f, 0.24f, 0.72f, 0.46f, 0.08f); return true; }
    if (is("refresh")) { p.Arc(0.50f, 0.50f, 0.28f, 0.10f, 0.90f, 0.08f); p.Tri(0.62f, 0.16f, 0.86f, 0.26f, 0.62f, 0.38f); return true; }
    if (is("folder-plus")) { p.FillRect(0.12f, 0.28f, 0.46f, 0.38f, 0.03f); p.Rect(0.12f, 0.32f, 0.88f, 0.76f, 0.08f, 0.05f); p.Line(0.50f, 0.44f, 0.50f, 0.66f, 0.07f); p.Line(0.39f, 0.55f, 0.61f, 0.55f, 0.07f); return true; }
    if (is("search")) { p.Circle(0.42f, 0.42f, 0.22f); p.Line(0.58f, 0.58f, 0.82f, 0.82f, 0.09f); return true; }
    if (is("code")) { p.Line(0.36f, 0.30f, 0.18f, 0.50f, 0.08f); p.Line(0.18f, 0.50f, 0.36f, 0.70f, 0.08f); p.Line(0.64f, 0.30f, 0.82f, 0.50f, 0.08f); p.Line(0.82f, 0.50f, 0.64f, 0.70f, 0.08f); p.Line(0.56f, 0.22f, 0.44f, 0.78f, 0.07f); return true; }
    if (is("question")) { p.Circle(0.50f, 0.50f, 0.34f); p.Arc(0.50f, 0.38f, 0.11f, 0.55f, 1.05f, 0.07f); p.Line(0.50f, 0.49f, 0.50f, 0.60f, 0.07f); p.Disc(0.50f, 0.71f, 0.045f); return true; }
    if (is("layout")) { p.Rect(0.12f, 0.16f, 0.88f, 0.84f, 0.05f, 0.05f); p.Line(0.50f, 0.16f, 0.50f, 0.84f, 0.05f); p.Line(0.12f, 0.50f, 0.88f, 0.50f, 0.05f); return true; }
    if (is("model"))  { p.Line(0.50f, 0.16f, 0.84f, 0.36f); p.Line(0.84f, 0.36f, 0.50f, 0.56f); p.Line(0.50f, 0.56f, 0.16f, 0.36f); p.Line(0.16f, 0.36f, 0.50f, 0.16f); p.Line(0.16f, 0.36f, 0.16f, 0.64f, 0.06f); p.Line(0.84f, 0.36f, 0.84f, 0.64f, 0.06f); p.Line(0.50f, 0.56f, 0.50f, 0.84f, 0.06f); p.Line(0.16f, 0.64f, 0.50f, 0.84f, 0.06f); p.Line(0.84f, 0.64f, 0.50f, 0.84f, 0.06f); return true; }

    // Уровни сообщений.
    if (is("warn"))  { p.Tri(0.50f, 0.14f, 0.10f, 0.84f, 0.90f, 0.84f); return true; }
    if (is("error")) { p.Disc(0.50f, 0.50f, 0.36f); return true; }
    if (is("info"))  { p.Circle(0.50f, 0.50f, 0.34f, 0.10f); p.Line(0.50f, 0.44f, 0.50f, 0.68f, 0.10f); p.Disc(0.50f, 0.34f, 0.06f); return true; }
    if (is("debug")) { p.Circle(0.50f, 0.50f, 0.22f, 0.08f); p.Line(0.22f, 0.50f, 0.10f, 0.50f, 0.06f); p.Line(0.78f, 0.50f, 0.90f, 0.50f, 0.06f); p.Line(0.50f, 0.22f, 0.50f, 0.10f, 0.06f); return true; }

    // Действия.
    if (is("trash")) { p.Rect(0.26f, 0.28f, 0.74f, 0.86f, 0.08f, 0.05f); p.Line(0.18f, 0.24f, 0.82f, 0.24f, 0.09f); p.Line(0.40f, 0.16f, 0.60f, 0.16f, 0.08f); return true; }
    if (is("copy"))  { p.Rect(0.14f, 0.14f, 0.62f, 0.62f, 0.08f, 0.05f); p.Rect(0.38f, 0.38f, 0.86f, 0.86f, 0.08f, 0.05f); return true; }
    if (is("save"))  { p.Rect(0.16f, 0.16f, 0.84f, 0.84f, 0.08f, 0.05f); p.FillRect(0.34f, 0.16f, 0.66f, 0.42f); p.Rect(0.30f, 0.58f, 0.70f, 0.84f, 0.06f); return true; }
    if (is("open"))  { p.Rect(0.12f, 0.28f, 0.88f, 0.80f, 0.08f, 0.05f); p.FillRect(0.12f, 0.20f, 0.44f, 0.30f, 0.03f); return true; }
    if (is("plus"))  { p.Line(0.50f, 0.18f, 0.50f, 0.82f, 0.12f); p.Line(0.18f, 0.50f, 0.82f, 0.50f, 0.12f); return true; }
    if (is("eye"))   { p.Arc(0.50f, 0.72f, 0.36f, 0.62f, 0.88f, 0.08f); p.Arc(0.50f, 0.28f, 0.36f, 0.12f, 0.38f, 0.08f); p.Disc(0.50f, 0.50f, 0.11f); return true; }
    if (is("lock"))  { p.Rect(0.26f, 0.46f, 0.74f, 0.84f, 0.09f, 0.06f); p.Arc(0.50f, 0.46f, 0.17f, 0.5f, 1.0f, 0.08f); return true; }

    return false;
}

// Заглушка для незнакомого имени: пустой квадрат с диагональю. Заметно, но не
// ломает вёрстку — ровно как у иконок игрового интерфейса.
void Unknown(const Pen& p) {
    p.Rect(0.16f, 0.16f, 0.84f, 0.84f, 0.08f);
    p.Line(0.16f, 0.16f, 0.84f, 0.84f, 0.07f);
}

void DrawAt(ImDrawList* dl, ImVec2 pos, float size, const char* icon, ImU32 color) {
    Pen pen{dl, pos, size, color};
    if (!Shape(pen, icon)) Unknown(pen);
}

} // namespace

bool Has(const char* icon) {
    // Формы рисуются, а не перечисляются, поэтому «есть ли иконка» проверяется
    // прогоном по невидимому списку команд: ImDrawList с нулевым размером
    // ничего не рисует, а Shape честно возвращает, знает ли он имя.
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl.PushClipRectFullScreen();
    dl.PushTextureID(ImGui::GetIO().Fonts->TexID);
    Pen pen{&dl, ImVec2(0, 0), 0.0f, 0};
    return Shape(pen, icon);
}

bool Button(const char* icon, const char* label, const char* tooltip, bool active) {
    const float h = ImGui::GetFrameHeight();
    const float icon_s = h * 0.68f;
    ImGui::PushID(label);
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.216f, 0.322f, 0.520f, 1.0f));

    // Место под иконку резервируется отступом слева — так подпись не наезжает
    // на рисунок при любом шрифте.
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const ImVec2 size(textSize.x + icon_s + pad.x * 3.0f, h);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button("##btn", size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    DrawAt(dl, ImVec2(cursor.x + pad.x, cursor.y + (h - icon_s) * 0.5f), icon_s, icon, col);
    dl->AddText(ImVec2(cursor.x + pad.x * 2.0f + icon_s, cursor.y + (h - textSize.y) * 0.5f), col,
                label);

    if (active) ImGui::PopStyleColor();
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

bool IconOnlyButton(const char* icon, const char* tooltip, bool active, const glm::vec3& tint) {
    const float h = ImGui::GetFrameHeight();
    ImGui::PushID(icon);
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.216f, 0.322f, 0.520f, 1.0f));
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button("##ibtn", ImVec2(h, h));
    const float icon_s = h * 0.62f;
    DrawAt(ImGui::GetWindowDrawList(),
           ImVec2(cursor.x + (h - icon_s) * 0.5f, cursor.y + (h - icon_s) * 0.5f), icon_s, icon,
           Col(tint));
    if (active) ImGui::PopStyleColor();
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

void Inline(const char* icon, const glm::vec3& color) {
    const float s = ImGui::GetTextLineHeight();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    DrawAt(ImGui::GetWindowDrawList(), ImVec2(cursor.x, cursor.y), s, icon, Col(color));
    // Место под иконку резервируется Dummy: без него следующий SameLine лёг бы
    // поверх рисунка, потому что ImGui о нарисованном напрямую не знает.
    ImGui::Dummy(ImVec2(s, s));
}

void Overlay(float x, float y, float size, const char* icon, const glm::vec3& color) {
    // Ни курсора, ни Dummy: рисунок ложится в список отрисовки окна, а «последний
    // элемент» ImGui остаётся тем, что подали до вызова.
    DrawAt(ImGui::GetWindowDrawList(), ImVec2(x, y), size, icon, Col(color));
}

} // namespace EditorIcons

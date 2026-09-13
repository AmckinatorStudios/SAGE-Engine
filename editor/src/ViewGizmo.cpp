#include "ViewGizmo.h"

#include <algorithm>
#include <cmath>

#include "sage/render/Camera.h"
#include "EditorTheme.h"
#include "Localization.h"

namespace sage::editor::viewgizmo {

namespace {

// Размер гизмо и отступ от угла. Достаточно крупный, чтобы попадать по шарику
// мышью не целясь, и достаточно скромный, чтобы не закрывать сцену.
constexpr float kRadius = 42.0f;
constexpr float kInset = 14.0f;
constexpr float kBall = 9.0f;
constexpr float kSnapSeconds = 0.25f;

struct Axis {
    glm::vec3 Dir;      // направление оси в мире
    const char* Label;  // подпись у положительного конца ("" — у отрицательного)
    ImU32 Color;
    bool Positive;
};

// Цвета осей — те же, что у гизмо перемещения: X красный, Y зелёный, Z синий.
// Разойтись им нельзя: человек читает «красное — это X» один раз и дальше
// полагается на это во всём редакторе.
ImU32 AxisColor(int axis, bool positive) {
    const ImU32 kBase[3] = {IM_COL32(226, 86, 86, 255), IM_COL32(122, 200, 96, 255),
                            IM_COL32(86, 140, 235, 255)};
    if (positive) return kBase[axis];
    // Отрицательный конец — тот же цвет, но приглушённый: это та же ось, а не
    // другая сущность, и красить её в серый значило бы потерять связь.
    const ImU32 c = kBase[axis];
    const int r = (int)((c >> IM_COL32_R_SHIFT) & 0xFF), g = (int)((c >> IM_COL32_G_SHIFT) & 0xFF),
              b = (int)((c >> IM_COL32_B_SHIFT) & 0xFF);
    return IM_COL32(r * 45 / 100, g * 45 / 100, b * 45 / 100, 255);
}

// Углы камеры, при которых она смотрит ВДОЛЬ front.
void AnglesFor(const glm::vec3& front, float& yaw, float& pitch) {
    pitch = glm::degrees(std::asin(std::clamp(front.y, -1.0f, 1.0f)));
    // У взгляда строго вверх/вниз поворота вокруг вертикали нет — atan2 от нулей
    // дал бы произвольный. Оставляем прежний, чтобы «сверху» не разворачивало
    // сцену вбок при каждом нажатии.
    if (std::abs(front.y) < 0.9999f) yaw = glm::degrees(std::atan2(front.z, front.x));
    // Ровно 90° ломает построение базиса (Front становится параллелен WorldUp).
    pitch = std::clamp(pitch, -89.9f, 89.9f);
}

// Кратчайший путь между углами: поворот с 179° на -179° обязан идти через 180,
// а не через весь круг обратно.
float ShortestDelta(float from, float to) {
    float d = std::fmod(to - from + 540.0f, 360.0f) - 180.0f;
    return d;
}

} // namespace

bool Draw(State& state, Camera& camera, const ImVec2& viewMin, const ImVec2& viewMax,
          const glm::vec3& pivot, float dt) {
    const ImVec2 center(viewMax.x - kRadius - kInset, viewMin.y + kRadius + kInset);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // --- Плавный переход к выбранной оси -----------------------------------
    if (state.Animating) {
        state.Time += (kSnapSeconds > 0.0f) ? dt / kSnapSeconds : 1.0f;
        const float t = std::clamp(state.Time, 0.0f, 1.0f);
        // Плавный вход и выход: линейный поворот начинается и кончается рывком.
        const float e = t * t * (3.0f - 2.0f * t);
        const float dist = glm::length(pivot - camera.Position);
        camera.SetAngles(state.FromYaw + ShortestDelta(state.FromYaw, state.ToYaw) * e,
                         state.FromPitch + (state.ToPitch - state.FromPitch) * e);
        // Расстояние до точки вращения сохраняется: гизмо поворачивает вид, а
        // не подъезжает и не отъезжает.
        camera.Position = state.Pivot - camera.Front * std::max(dist, 0.01f);
        if (t >= 1.0f) state.Animating = false;
    }

    // --- Экранные положения концов осей ------------------------------------
    struct Dot {
        ImVec2 At;
        float Depth;     // чем больше, тем ДАЛЬШЕ от зрителя
        ImU32 Color;
        const char* Label;
        glm::vec3 Front; // куда смотреть, если по нему щёлкнуть
    };
    Dot dots[6];
    const char* kLabels[3] = {"X", "Y", "Z"};
    int n = 0;
    for (int a = 0; a < 3; ++a) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            glm::vec3 dir(0.0f);
            dir[a] = (float)sign;
            // Ось раскладывается по базису камеры: вправо по экрану — это
            // Right, вверх — Up (экранная Y растёт вниз, отсюда минус).
            const float x = glm::dot(dir, camera.Right);
            const float y = glm::dot(dir, camera.Up);
            const float z = glm::dot(dir, camera.Front);
            dots[n].At = ImVec2(center.x + x * kRadius, center.y - y * kRadius);
            dots[n].Depth = z;
            dots[n].Color = AxisColor(a, sign > 0);
            dots[n].Label = sign > 0 ? kLabels[a] : "";
            dots[n].Front = -dir;   // щёлкнули по +X — значит смотрим из +X, то есть в -X
            ++n;
        }
    }
    // Дальние рисуются первыми: иначе ближний шарик оказался бы под дальним, и
    // объёма не читалось бы.
    std::sort(dots, dots + n, [](const Dot& a, const Dot& b) { return a.Depth > b.Depth; });

    // --- Мышь ---------------------------------------------------------------
    const ImVec2 mouse = ImGui::GetMousePos();
    const float dx = mouse.x - center.x, dy = mouse.y - center.y;
    const bool overGizmo = (dx * dx + dy * dy) <= (kRadius + kBall) * (kRadius + kBall);
    const bool viewHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                             mouse.x >= viewMin.x && mouse.x <= viewMax.x &&
                             mouse.y >= viewMin.y && mouse.y <= viewMax.y;

    int hovered = -1;
    if (overGizmo && viewHovered && !state.Dragging) {
        float best = kBall * kBall * 2.25f;
        for (int i = 0; i < n; ++i) {
            const float ddx = mouse.x - dots[i].At.x, ddy = mouse.y - dots[i].At.y;
            const float d2 = ddx * ddx + ddy * ddy;
            // При равном расстоянии выигрывает БЛИЖНИЙ: под курсором тот шарик,
            // который человек видит, а не спрятанный за ним.
            if (d2 <= best) { best = d2; hovered = i; }
        }
    }

    // Перетаскивание — вращение вокруг точки внимания.
    if (overGizmo && viewHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.Dragging = true;
        state.Animating = false;
        state.Pivot = pivot;
    }
    if (state.Dragging) {
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            state.Dragging = false;
            // Щелчок без движения по шарику = «встать на эту ось». Различаем по
            // тому же порогу, что и рамка выделения: рука дрожит.
            const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            if (std::abs(drag.x) < 4.0f && std::abs(drag.y) < 4.0f && hovered >= 0) {
                state.Animating = true;
                state.Time = 0.0f;
                state.FromYaw = camera.Yaw;
                state.FromPitch = camera.Pitch;
                state.ToYaw = camera.Yaw;
                state.ToPitch = camera.Pitch;
                AnglesFor(dots[hovered].Front, state.ToYaw, state.ToPitch);
                state.Pivot = pivot;
            }
        } else {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            if (delta.x != 0.0f || delta.y != 0.0f) {
                const float dist = glm::length(state.Pivot - camera.Position);
                camera.SetAngles(camera.Yaw + delta.x * 0.4f, camera.Pitch - delta.y * 0.4f);
                camera.Position = state.Pivot - camera.Front * std::max(dist, 0.01f);
            }
        }
    }

    // --- Рисунок ------------------------------------------------------------
    // Подложка — чтобы гизмо читалось и на светлой сцене, и на тёмной. Появляется
    // заметнее под курсором: в покое она не должна спорить с картинкой.
    const float bgAlpha = (overGizmo && viewHovered) ? 0.38f : 0.18f;
    dl->AddCircleFilled(center, kRadius + kBall + 2.0f, ImGui::GetColorU32(ImVec4(0, 0, 0, bgAlpha)),
                        48);

    for (int i = 0; i < n; ++i) {
        const bool positive = dots[i].Label[0] != '\0';
        const bool isHovered = (hovered == i);
        // Палочка — только у положительных концов: шесть палочек из центра
        // превращают гизмо в ёжика, по которому ничего не прочитать.
        if (positive) {
            dl->AddLine(center, dots[i].At, dots[i].Color, 2.0f);
        }
        const float r = isHovered ? kBall + 2.0f : kBall;
        if (positive || isHovered) {
            dl->AddCircleFilled(dots[i].At, r, dots[i].Color, 20);
        } else {
            // Отрицательный конец — кольцо: та же ось, но «с той стороны».
            dl->AddCircleFilled(dots[i].At, r, IM_COL32(0, 0, 0, 120), 20);
            dl->AddCircle(dots[i].At, r, dots[i].Color, 20, 2.0f);
        }
        if (positive) {
            const ImVec2 ts = ImGui::CalcTextSize(dots[i].Label);
            dl->AddText(ImVec2(dots[i].At.x - ts.x * 0.5f, dots[i].At.y - ts.y * 0.5f),
                        IM_COL32(16, 18, 22, 255), dots[i].Label);
        }
    }

    if (hovered >= 0) {
        ImGui::SetTooltip("%s", T("Click — look along this axis; drag — orbit the view"));
    }

    // Мышь занята гизмо, пока она над ним или пока его крутят: иначе тот же
    // щелчок ещё и выбирал бы объект под гизмо.
    return state.Dragging || (overGizmo && viewHovered);
}

} // namespace sage::editor::viewgizmo

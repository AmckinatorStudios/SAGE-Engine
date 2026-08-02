#include "ViewportPanel.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "imgui.h"
#include "ImGuizmo.h"

#include "EditorHost.h"
#include "sage/core/Application.h"
#include "sage/scene/Components.h"

namespace {

// Раскладывает мировую матрицу обратно в Transform (Position/Rotation/Scale).
// ВАЖНО: порядок углов должен совпадать с Transform::GetMatrix (T*Rx*Ry*Rz*S),
// поэтому используется glm::extractEulerAngleXYZ, а не декомпозиция ImGuizmo
// (у неё другой порядок осей — гизмо «прыгал» бы на повёрнутых объектах).
void DecomposeToTransform(const glm::mat4& m, Transform& out) {
    out.Position = glm::vec3(m[3]);

    glm::vec3 scale(glm::length(glm::vec3(m[0])),
                    glm::length(glm::vec3(m[1])),
                    glm::length(glm::vec3(m[2])));
    scale = glm::max(scale, glm::vec3(1e-6f)); // защита от вырожденного масштаба
    out.Scale = scale;

    glm::mat4 rot(1.0f);
    rot[0] = glm::vec4(glm::vec3(m[0]) / scale.x, 0.0f);
    rot[1] = glm::vec4(glm::vec3(m[1]) / scale.y, 0.0f);
    rot[2] = glm::vec4(glm::vec3(m[2]) / scale.z, 0.0f);

    float rx, ry, rz;
    glm::extractEulerAngleXYZ(rot, rx, ry, rz);
    out.Rotation = glm::degrees(glm::vec3(rx, ry, rz));
}

} // namespace


const char* ViewportPanel::ViewKindName(ViewKind kind) {
    switch (kind) {
        case ViewKind::Perspective: return "Перспектива";
        case ViewKind::Top:         return "Сверху";
        case ViewKind::Front:       return "Спереди";
        case ViewKind::Side:        return "Сбоку";
    }
    return "?";
}

// Матрица взгляда ортогонального вида. Камера ставится ДАЛЕКО от центра, а не
// «в бесконечности»: ближняя и дальняя плоскости конечны, и объект, оказавшийся
// за камерой, просто исчез бы — самый обидный вид пропажи, потому что он
// выглядит как «объект удалился».
glm::mat4 ViewportPanel::OrthoViewMatrix(ViewKind kind, const glm::vec3& center) {
    constexpr float kDist = 500.0f;
    switch (kind) {
        case ViewKind::Top:
            // Смотрим вниз; «верх» экрана — мировой -Z, чтобы вид сверху
            // читался как карта: X вправо, Z вниз.
            return glm::lookAt(center + glm::vec3(0.0f, kDist, 0.0f), center,
                               glm::vec3(0.0f, 0.0f, -1.0f));
        case ViewKind::Front:
            return glm::lookAt(center + glm::vec3(0.0f, 0.0f, kDist), center,
                               glm::vec3(0.0f, 1.0f, 0.0f));
        case ViewKind::Side:
            return glm::lookAt(center + glm::vec3(kDist, 0.0f, 0.0f), center,
                               glm::vec3(0.0f, 1.0f, 0.0f));
        default:
            return glm::mat4(1.0f);
    }
}

void ViewportPanel::DrawViewToolbar(EditorHost& host) {
    (void)host;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 3));
    ImGui::BeginChild("##viewbar", ImVec2(0, ImGui::GetFrameHeight() + 8), false);

    const char* layouts[] = {"Один вид", "Два столбца", "Четыре вида"};
    int layout = (int)m_layout;
    ImGui::SetNextItemWidth(150);
    if (ImGui::Combo("##layout", &layout, layouts, IM_ARRAYSIZE(layouts))) {
        m_layout = (Layout)layout;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Раскладка вьюпорта.\nКаждый вид — полный проход сцены,\n"
                          "поэтому лишние виды стоят кадров.");
    }

    // Вид активного слота: перспектива или одна из ортогональных проекций.
    ImGui::SameLine();
    const char* kinds[] = {"Перспектива", "Сверху", "Спереди", "Сбоку"};
    int kind = (int)m_kinds[m_activeSlot];
    ImGui::SetNextItemWidth(140);
    if (ImGui::Combo("##kind", &kind, kinds, IM_ARRAYSIZE(kinds))) {
        m_kinds[m_activeSlot] = (ViewKind)kind;
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Показать всё")) {
        // Вписываем сцену в ортогональные виды: без этого человек, отъехавший
        // колесом далеко, обратно уже не найдёт дорогу.
        for (OrthoView& v : m_ortho) { v.Center = glm::vec3(0.0f); v.Height = 20.0f; }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("| активен: %s", ViewKindName(m_kinds[m_activeSlot]));

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void ViewportPanel::Draw(EditorHost& host) {
    // Раскладка из переменной окружения — для headless-прогонов и скриншотов
    // документации: кликнуть в комбо там некому, а проверять раскладку надо.
    static bool layoutFromEnv = false;
    if (!layoutFromEnv) {
        layoutFromEnv = true;
        if (const char* v = std::getenv("SAGE_EDITOR_VIEW_LAYOUT")) {
            const std::string mode = v;
            if (mode == "quad") m_layout = Layout::Quad;
            else if (mode == "two") m_layout = Layout::TwoColumns;
        }
    }
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus(); // вывести Viewport вперёд на старте (см. RequestFocus)
        --m_focusFrames;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");

    DrawViewToolbar(host);

    // --- Раскладка -----------------------------------------------------------
    //
    // Слот 0 остаётся ГЛАВНЫМ: в нём живут гизмо, пикинг и полёт камеры, и он
    // же — единственный при одиночной раскладке. Дополнительные виды
    // показывают ту же сцену с других сторон и служат для проверки, а не для
    // манипуляции: два гизмо в двух видах одновременно — это два разных ответа
    // на вопрос «куда я тащу», и выбрать между ними нечем.
    const int viewCount = m_layout == Layout::Single ? 1 : (m_layout == Layout::TwoColumns ? 2 : 4);
    const ImVec2 full = ImGui::GetContentRegionAvail();
    const float gap = 2.0f;
    ImVec2 cell = full;
    if (viewCount == 2) cell.x = (full.x - gap) * 0.5f;
    else if (viewCount == 4) { cell.x = (full.x - gap) * 0.5f; cell.y = (full.y - gap) * 0.5f; }
    if (cell.x < 8 || cell.y < 8) { ImGui::End(); ImGui::PopStyleVar(); return; }

    EditorHost::ViewRequest requests[EditorHost::kMaxViews];
    ImVec2 slotPos[EditorHost::kMaxViews];
    bool slotHovered[EditorHost::kMaxViews] = {false, false, false, false};

    for (int i = 0; i < viewCount; ++i) {
        if (i > 0 && (viewCount == 2 || i % 2 == 1)) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);
        ImGui::BeginChild("##view", cell, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        slotHovered[i] = ImGui::IsWindowHovered();
        slotPos[i] = ImGui::GetCursorScreenPos();

        const ViewKind kind = m_kinds[i];
        const bool ortho = kind != ViewKind::Perspective;

        requests[i].Active = true;
        requests[i].W = (int)cell.x;
        requests[i].H = (int)cell.y;
        requests[i].Ortho = ortho;
        if (ortho) {
            const float aspect = cell.x / std::max(cell.y, 1.0f);
            const float halfH = m_ortho[i].Height * 0.5f;
            const float halfW = halfH * aspect;
            requests[i].View = OrthoViewMatrix(kind, m_ortho[i].Center);
            requests[i].Proj = glm::ortho(-halfW, halfW, -halfH, halfH, 0.1f, 1000.0f);
            requests[i].EyePos = m_ortho[i].Center;
        }

        const uint64_t texId = (i == 0) ? host.SceneTexture() : host.ViewTexture(i);
        if (texId) {
            ImGui::Image((ImTextureID)(std::intptr_t)texId, cell, ImVec2(0, 1), ImVec2(1, 0));
        } else {
            ImGui::Dummy(cell);
        }

        // Подпись вида поверх картинки и рамка активного: без них в четырёх
        // одинаковых серых прямоугольниках невозможно понять, где что.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(slotPos[i].x + 8, slotPos[i].y + 6),
                    i == m_activeSlot ? IM_COL32(255, 210, 120, 230) : IM_COL32(190, 195, 205, 190),
                    ViewKindName(kind));
        if (viewCount > 1 && i == m_activeSlot) {
            dl->AddRect(slotPos[i], ImVec2(slotPos[i].x + cell.x, slotPos[i].y + cell.y),
                        IM_COL32(255, 175, 60, 200), 0.0f, 0, 2.0f);
        }

        // Клик по виду делает его активным — дальше в нём работают гизмо и
        // хоткеи. Без этого в раскладке из четырёх видов работал бы только один.
        if (slotHovered[i] && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_activeSlot = i;

        // Ортогональный вид: колесо — масштаб, средняя кнопка — панорама.
        if (ortho && slotHovered[i]) {
            ImGuiIO& vio = ImGui::GetIO();
            if (vio.MouseWheel != 0.0f) {
                m_ortho[i].Height =
                    std::max(0.5f, m_ortho[i].Height * (vio.MouseWheel > 0 ? 0.88f : 1.14f));
            }
            const bool panning = ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                                 (ImGui::IsMouseDown(ImGuiMouseButton_Right));
            if (panning && (vio.MouseDelta.x != 0.0f || vio.MouseDelta.y != 0.0f)) {
                // Перевод пикселей в мир через ту же высоту кадра — иначе
                // панорама «убегает» при смене масштаба.
                const float perPixel = m_ortho[i].Height / std::max(cell.y, 1.0f);
                const float dx = -vio.MouseDelta.x * perPixel;
                const float dy = vio.MouseDelta.y * perPixel;
                switch (kind) {
                    case ViewKind::Top:   m_ortho[i].Center += glm::vec3(dx, 0.0f, -dy); break;
                    case ViewKind::Front: m_ortho[i].Center += glm::vec3(dx, dy, 0.0f); break;
                    case ViewKind::Side:  m_ortho[i].Center += glm::vec3(0.0f, dy, -dx); break;
                    default: break;
                }
            }
        }

        ImGui::EndChild();
        ImGui::PopID();
    }
    host.SetViewRequests(requests, viewCount);

    // Дальше — интерактив ГЛАВНОГО слота: гизмо, пикинг, полёт камеры.
    const bool hovered = slotHovered[m_activeSlot] && m_kinds[m_activeSlot] == ViewKind::Perspective;
    const ImVec2 imgPos = slotPos[m_activeSlot];
    const ImVec2 avail = cell;
    if (m_activeSlot == 0 && m_kinds[0] == ViewKind::Perspective) {
        host.SetViewportSize((int)cell.x, (int)cell.y);
    }

    ImGuiIO& io = ImGui::GetIO();
    float dt = sage::Application::Get().DeltaTime();
    Camera& camera = host.EditorCamera();

    // --- Камера: ПКМ — осмотр, ПКМ+WASDQE — полёт, Shift — ускорение ---
    bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if ((hovered || m_cameraDriving) && rmb) {
        m_cameraDriving = true;
        camera.ProcessMouse(io.MouseDelta.x, -io.MouseDelta.y);
        float speed = camera.MovementSpeed * (io.KeyShift ? 3.0f : 1.0f) * dt;
        if (ImGui::IsKeyDown(ImGuiKey_W)) camera.Position += camera.Front * speed;
        if (ImGui::IsKeyDown(ImGuiKey_S)) camera.Position -= camera.Front * speed;
        if (ImGui::IsKeyDown(ImGuiKey_A)) camera.Position -= camera.Right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_D)) camera.Position += camera.Right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_E)) camera.Position += camera.WorldUp * speed;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) camera.Position -= camera.WorldUp * speed;
    } else {
        m_cameraDriving = false;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        camera.Position += camera.Front * io.MouseWheel * 0.8f;
    }

    // --- Хоткеи гизмо (не во время полёта камеры и не в полях ввода) ---
    if (hovered && !m_cameraDriving && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) host.GizmoOp() = (int)ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) host.GizmoOp() = (int)ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) host.GizmoOp() = (int)ImGuizmo::SCALE;
    }

    // --- ImGuizmo: манипулятор выбранной сущности (сетка — DebugDraw в FBO) ---
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(imgPos.x, imgPos.y, avail.x, avail.y);

    GameObject selected = host.SelectedObject();
    if (selected.Valid()) {
        Transform& tr = selected.GetTransform();
        // Гизмо работает в МИРОВОМ пространстве (учёт родителей): манипулируем
        // мировой матрицей, результат переводим обратно в локальную через
        // inverse(родительская мировая). Для корневой сущности родитель = единица.
        Scene& scene = host.CurrentScene();
        entt::entity parent = scene.ParentOf(selected.Entity());
        glm::mat4 parentWorld = (parent != entt::null) ? scene.WorldMatrix(parent) : glm::mat4(1.0f);
        glm::mat4 model = scene.WorldMatrix(selected.Entity());

        // Пока гизмо не тащат, но курсор над ним — запоминаем состояние «до»:
        // первый же кадр перетаскивания уже мутирует Transform, поэтому снапшот
        // должен быть взят раньше него.
        if (!ImGuizmo::IsUsing() && ImGuizmo::IsOver() && !host.InPlayMode()) {
            host.CapturePendingSnapshot();
        }

        float snapT = 0.5f, snapR = 15.0f, snapS = 0.1f;
        float snapValues[3];
        auto op = (ImGuizmo::OPERATION)host.GizmoOp();
        float snapUnit = (op == ImGuizmo::ROTATE) ? snapR : (op == ImGuizmo::SCALE ? snapS : snapT);
        snapValues[0] = snapValues[1] = snapValues[2] = snapUnit;

        // Scale всегда в локальном пространстве (ImGuizmo игнорит WORLD для scale);
        // Move/Rotate — по выбору пользователя (тулбар: Local/World).
        auto mode = (host.GizmoSpace() == EditorGizmoSpace::World && op != ImGuizmo::SCALE)
                        ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        // Фронт «начали таскать»: одна запись undo + снимок мировых матриц всех
        // выбранных (для мультивыделения — двигаем весь набор относительно
        // первичной, вокруг которой и стоит гизмо).
        bool usingNow = ImGuizmo::IsUsing();
        if (usingNow && !m_gizmoWasUsing && !host.InPlayMode()) {
            host.CommitPendingSnapshot();
            m_dragStartPrimary = scene.WorldMatrix(selected.Entity());
            m_dragStartWorlds.clear();
            for (int id : host.Selection()) {
                GameObject o = scene.Get(id);
                if (o.Valid()) m_dragStartWorlds.push_back({id, scene.WorldMatrix(o.Entity())});
            }
        }

        if (ImGuizmo::Manipulate(glm::value_ptr(host.ViewMatrix()), glm::value_ptr(host.ProjMatrix()),
                                 op, mode, glm::value_ptr(model),
                                 nullptr, host.GizmoSnap() ? snapValues : nullptr)) {
            // Мировая -> локальная: убираем вклад родителя.
            glm::mat4 local = (parent != entt::null) ? glm::inverse(parentWorld) * model : model;
            DecomposeToTransform(local, tr);

            // Мультивыделение: та же мировая дельта — на остальные выбранные.
            if (host.Selection().size() > 1) {
                glm::mat4 delta = model * glm::inverse(m_dragStartPrimary);
                for (auto& [id, startWorld] : m_dragStartWorlds) {
                    if (id == selected.Id()) continue;
                    GameObject o = scene.Get(id);
                    if (!o.Valid()) continue;
                    // Пропускаем потомков других выбранных: их несёт двигающийся
                    // родитель, иначе смещение применилось бы дважды.
                    bool ancestorSelected = false;
                    for (entt::entity a = scene.ParentOf(o.Entity());
                         a != entt::null; a = scene.ParentOf(a)) {
                        if (host.IsSelected(scene.Registry().get<IdComponent>(a).Id)) {
                            ancestorSelected = true;
                            break;
                        }
                    }
                    if (ancestorSelected) continue;
                    glm::mat4 newWorld = delta * startWorld;
                    entt::entity p = scene.ParentOf(o.Entity());
                    glm::mat4 pw = (p != entt::null) ? scene.WorldMatrix(p) : glm::mat4(1.0f);
                    glm::mat4 loc = (p != entt::null) ? glm::inverse(pw) * newWorld : newWorld;
                    DecomposeToTransform(loc, o.GetTransform());
                }
            }
        }
        m_gizmoWasUsing = usingNow;
    } else {
        m_gizmoWasUsing = false;
    }

    // --- Пикинг ЛКМ (не по гизмо и не во время манипуляции) ---
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
        ImVec2 mp = ImGui::GetMousePos();
        float u = (mp.x - imgPos.x) / avail.x;
        float v = (mp.y - imgPos.y) / avail.y;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)
            host.PickAtViewport(u, v, io.KeyCtrl); // Ctrl — добавить/убрать из набора
    }

    // Инструменты (режим гизмо, snap, пространство, Play, режим рендера) —
    // в верхнем тулбаре редактора (ToolbarPanel), не оверлеем во вьюпорте.

    ImGui::End(); // Viewport
    ImGui::PopStyleVar();
}

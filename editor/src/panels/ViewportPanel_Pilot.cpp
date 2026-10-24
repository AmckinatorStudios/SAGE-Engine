#include "sage/scene/ScaleLock.h"
#include "../DragSet.h"
#include "../PanelWindows.h"
#include "ViewportPanel.h"
#include "ModelImportDialog.h"

#include <cmath>
#include <filesystem>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "ImGuizmo.h"

#include "EditorHost.h"
#include "sage/app/Application.h"
#include "sage/scene/Components.h"
#include "sage/ecs/LightSystem.h"
#include "../Localization.h"
#include "EditorIcons.h"
#include "../EditorPrefs.h"



// Вид от лица камеры сцены (пилот) и её превью в углу вьюпорта.
// ---------------------------------------------------------------------------
// УПРАВЛЕНИЕ КАМЕРОЙ ОТ ЕЁ ЛИЦА (см. ViewportPanel.h)
// ---------------------------------------------------------------------------

void ViewportPanel::StartPilot(EditorHost& host, int cameraId) {
    GameObject cam = host.CurrentScene().Get(cameraId);
    if (!cam.Valid()) return;

    // Куда вернуть вид редактора после выхода.
    Camera& view = host.EditorCamera();
    m_pilotReturnPos = view.Position;
    m_pilotReturnYaw = view.Yaw;
    m_pilotReturnPitch = view.Pitch;
    m_pilotReturnFov = view.Fov;

    // Вид редактора встаёт РОВНО НА камеру: угол обзора берётся её же, иначе
    // «то, что вижу» и «то, что снимет камера» отличались бы по краям кадра —
    // а наводят как раз по краям.
    const glm::mat4 world = host.CurrentScene().WorldMatrix(cam.Entity());
    const glm::vec3 fwd =
        glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    float yaw = view.Yaw;
    float pitch = glm::degrees(std::asin(std::clamp(fwd.y, -1.0f, 1.0f)));
    // Взгляд строго вверх или вниз не задаёт поворота вокруг вертикали —
    // atan2 от двух нулей дал бы произвольный, и камера развернулась бы вбок.
    if (std::abs(fwd.y) < 0.9999f) yaw = glm::degrees(std::atan2(fwd.z, fwd.x));
    view.Position = glm::vec3(world[3]);
    view.SetAngles(yaw, pitch);
    if (const CameraComponent* cc =
            host.CurrentScene().Registry().try_get<CameraComponent>(cam.Entity())) {
        view.Fov = cc->Fov;
    }

    // Одна запись отмены на весь полёт: каждый кадр управления меняет
    // положение, и снимок на кадр превратил бы историю в тысячу шагов.
    host.History().CapturePending();
    m_pilotId = cameraId;
}


void ViewportPanel::StopPilot(EditorHost& host) {
    if (m_pilotId < 0) return;
    m_pilotId = -1;
    Camera& view = host.EditorCamera();
    view.Position = m_pilotReturnPos;
    view.SetAngles(m_pilotReturnYaw, m_pilotReturnPitch);
    view.Fov = m_pilotReturnFov;
    host.History().CommitPending();
}


void ViewportPanel::SyncPilotCamera(EditorHost& host) {
    if (m_pilotId < 0) return;
    Scene& scene = host.CurrentScene();
    GameObject cam = scene.Get(m_pilotId);
    // Камеру могли удалить, снять с неё компонент или загрузить другую сцену.
    if (!cam.Valid() || !scene.Registry().all_of<CameraComponent>(cam.Entity()) ||
        host.InPlayMode()) {
        StopPilot(host);
        return;
    }

    // Базис вида редактора как мировая матрица камеры-объекта. Третий столбец —
    // МИНУС Front: камера смотрит вдоль своего -Z (та же договорённость, что у
    // sage::ecs::CameraFrameFor и у гизмо камеры в сцене).
    const Camera& view = host.EditorCamera();
    glm::mat4 world(1.0f);
    world[0] = glm::vec4(view.Right, 0.0f);
    world[1] = glm::vec4(view.Up, 0.0f);
    world[2] = glm::vec4(-view.Front, 0.0f);
    world[3] = glm::vec4(view.Position, 1.0f);

    // У дочерней камеры правится ЛОКАЛЬНЫЙ трансформ: вклад родителя убираем,
    // иначе камера, подвешенная к машине, уехала бы вместе с ней вдвое.
    const entt::entity parent = scene.ParentOf(cam.Entity());
    const glm::mat4 local =
        (parent != entt::null) ? glm::inverse(scene.WorldMatrix(parent)) * world : world;

    Transform& tr = cam.GetTransform();
    const glm::vec3 keepScale = tr.Scale;   // управление задаёт вид, а не размер
    tr.SetFromMatrix(local);   // тот же порядок углов, что у GetMatrix (Transform.h)
    tr.Scale = keepScale;
}


// ---------------------------------------------------------------------------
// КАРТОЧКА ПРЕВЬЮ ВЫБРАННОЙ КАМЕРЫ (см. ViewportPanel.h)
// ---------------------------------------------------------------------------

void ViewportPanel::DrawCameraPreview(EditorHost& host, ImVec2 viewMin, ImVec2 viewMax) {
    // Карточки нет — и её ректа нет. Оставленный от прошлого показа, он навсегда
    // забрал бы себе клики в этом углу вьюпорта.
    m_previewMin = m_previewMax = ImVec2(0.0f, 0.0f);
    if (host.InPlayMode()) return;

    // Показываем для ВЫБРАННОЙ камеры, а во время управления — для управляемой:
    // человек, который наводит кадр, обязательно щёлкнет по сцене мимо камеры,
    // и карточка не имеет права исчезнуть у него из-под рук посреди полёта.
    Scene& scene = host.CurrentScene();
    GameObject cam = (m_pilotId >= 0) ? scene.Get(m_pilotId) : host.SelectedObject();
    if (!cam.Valid() || !scene.Registry().all_of<CameraComponent>(cam.Entity())) return;

    if (!m_previewCornerLoaded) {
        m_previewCornerLoaded = true;
        m_previewCorner = std::clamp(sage::editor::prefs::GetInt("viewport.preview_corner", 3), 0, 3);
    }

    // Размер — доля ширины вида в пределах разумного: на узкой панели карточка
    // не должна закрывать половину сцены, на широкой — не должна превращаться
    // в марку, по которой кадр не разглядеть.
    const float w = std::clamp(std::floor((viewMax.x - viewMin.x) * 0.26f), 180.0f, 420.0f);
    const float h = std::floor(w * 9.0f / 16.0f);
    const float pad = 10.0f;
    const float caption = ImGui::GetFrameHeight() + 6.0f;
    const ImVec2 box(w + 12.0f, h + caption + 10.0f);

    // Просим кадр на СЛЕДУЮЩИЙ: запрос уходит хосту, а рисуется он в начале
    // кадра, ещё до панелей. Один кадр запаздывания на неподвижной камере не
    // виден вовсе, а лишнего прохода сцены это стоить не должно.
    host.RequestCameraPreview(cam.Id(), (int)w, (int)h);

    auto cornerPos = [&](int corner) {
        const float left = viewMin.x + pad;
        const float right = viewMax.x - box.x - pad;
        // Сверху слева стоит строка инструментов, а сверху справа — гизмо осей:
        // карточка в этих углах начинается ниже них.
        const float top = viewMin.y + pad + ((corner <= 1) ? 46.0f : 0.0f);
        const float bottom = viewMax.y - box.y - pad;
        switch (corner) {
            case 0: return ImVec2(left, top);
            case 1: return ImVec2(right, top);
            case 2: return ImVec2(left, bottom);
            default: return ImVec2(right, bottom);
        }
    };

    const ImVec2 pos = m_previewDragging ? m_previewDragPos : cornerPos(m_previewCorner);

    ImGui::SetCursorScreenPos(pos);
    const bool piloting = (m_pilotId == cam.Id());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.13f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, piloting ? ImVec4(1.0f, 0.62f, 0.12f, 0.95f)
                                                    : ImVec4(0.30f, 0.32f, 0.38f, 0.9f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, piloting ? 2.0f : 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 5));
    ImGui::BeginChild("##campreview", box, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoNavFocus);

    // --- Заголовок: он же РУЧКА ПЕРЕТАСКИВАНИЯ ------------------------------
    const ImVec2 gripStart = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##grip", ImVec2(w - ImGui::GetFrameHeight() * 2.2f,
                                            ImGui::GetFrameHeight()));
    const bool gripHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (!m_previewDragging) {
            m_previewDragging = true;
            m_previewDragPos = pos;
        }
        m_previewDragPos.x += ImGui::GetIO().MouseDelta.x;
        m_previewDragPos.y += ImGui::GetIO().MouseDelta.y;
    } else if (m_previewDragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // ОТПУСТИЛИ — ПРИЛИПАЕТ К БЛИЖАЙШЕМУ УГЛУ. Карточка, оставленная
        // посреди кадра, закрывает ровно то место, ради которого её и двигали;
        // а четыре угла — это весь выбор, который тут нужен.
        m_previewDragging = false;
        const float cx = m_previewDragPos.x + box.x * 0.5f;
        const float cy = m_previewDragPos.y + box.y * 0.5f;
        const bool right = cx > (viewMin.x + viewMax.x) * 0.5f;
        const bool bottom = cy > (viewMin.y + viewMax.y) * 0.5f;
        m_previewCorner = (bottom ? 2 : 0) + (right ? 1 : 0);
        sage::editor::prefs::SetInt("viewport.preview_corner", m_previewCorner);
    }
    if (gripHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

    // Имя камеры поверх ручки — подписью, а не отдельной строкой: строка стоила
    // бы высоты, которой у карточки в углу нет.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    EditorIcons::Overlay(dl, gripStart.x, gripStart.y + 2.0f, ImGui::GetTextLineHeight(), "camera",
                         glm::vec3(0.86f, 0.90f, 0.98f));
    const float textX = gripStart.x + ImGui::GetTextLineHeight() + 6.0f;
    dl->AddText(ImVec2(textX, gripStart.y + 3.0f),
                ImGui::GetColorU32(piloting ? ImGuiCol_NavHighlight : ImGuiCol_Text),
                cam.Name().c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("What this camera sees. Drag the card to any corner"));
    }

    // --- Кнопка «управлять» --------------------------------------------------
    ImGui::SameLine();
    ImGui::SetCursorPosX(box.x - ImGui::GetFrameHeight() * 2.0f);
    if (EditorIcons::IconOnlyButton(piloting ? "stop" : "pilot",
                                    piloting ? T("Stop controlling (Esc): the editor view goes "
                                                 "back where it was")
                                             : T("Fly this camera like the editor view: right "
                                                 "mouse button + WASD"),
                                    piloting)) {
        if (piloting) StopPilot(host);
        else StartPilot(host, cam.Id());
    }

    // --- Сам кадр ------------------------------------------------------------
    const uint64_t tex = host.CameraPreviewTexture();
    const ImVec2 imgPos = ImGui::GetCursorScreenPos();
    if (tex) {
        ImGui::Image((ImTextureID)(std::intptr_t)tex, ImVec2(w, h), ImVec2(0, 1), ImVec2(1, 0));
    } else {
        // Кадра ещё нет (первый кадр после выбора) — место под него занимаем
        // сразу, иначе карточка прыгает в размере.
        ImGui::Dummy(ImVec2(w, h));
    }
    ImGui::GetWindowDrawList()->AddRect(imgPos, ImVec2(imgPos.x + w, imgPos.y + h),
                                        IM_COL32(0, 0, 0, 120), 3.0f);

    // Рект карточки — для СЛЕДУЮЩЕГО кадра, по тому же правилу, что у строки
    // инструментов: мышь над ней не выбирает объект под ней и не хватает гизмо.
    m_previewMin = ImGui::GetWindowPos();
    m_previewMax = ImVec2(m_previewMin.x + ImGui::GetWindowSize().x,
                          m_previewMin.y + ImGui::GetWindowSize().y);

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

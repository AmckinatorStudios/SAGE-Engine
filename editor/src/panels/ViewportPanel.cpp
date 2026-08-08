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
#include "../Localization.h"

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
        case ViewKind::Perspective: return T("Perspective");
        case ViewKind::Top:         return T("Top");
        case ViewKind::Front:       return T("Front");
        case ViewKind::Side:        return T("Side");
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

    const char* layouts[] = {T("Single view"), T("Two columns"), T("Four views")};
    int layout = (int)m_layout;
    ImGui::SetNextItemWidth(150);
    if (ImGui::Combo("##layout", &layout, layouts, IM_ARRAYSIZE(layouts))) {
        m_layout = (Layout)layout;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Viewport layout.\n"
          "Each view is a full scene pass,\n"
          "so extra views cost frames."));
    }

    // Вид активного слота: перспектива или одна из ортогональных проекций.
    ImGui::SameLine();
    const char* kinds[] = {T("Perspective"), T("Top"), T("Front"), T("Side")};
    int kind = (int)m_kinds[m_activeSlot];
    ImGui::SetNextItemWidth(140);
    if (ImGui::Combo("##kind", &kind, kinds, IM_ARRAYSIZE(kinds))) {
        m_kinds[m_activeSlot] = (ViewKind)kind;
    }

    ImGui::SameLine();
    if (ImGui::SmallButton(T("Show all"))) {
        // Вписываем сцену в ортогональные виды: без этого человек, отъехавший
        // колесом далеко, обратно уже не найдёт дорогу.
        for (OrthoView& v : m_ortho) { v.Center = glm::vec3(0.0f); v.Height = 20.0f; }
    }
    ImGui::SameLine();
    ImGui::TextDisabled(T("| active: %s"), ViewKindName(m_kinds[m_activeSlot]));

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void ViewportPanel::Draw(EditorHost& host, bool* open) {
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
    ImGui::Begin(T("Viewport" "###Viewport"), open);

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
    // Список отрисовки КАЖДОГО слота. Нужен гизмо: рисовать его в список окна
    // Viewport нельзя — дочерние окна ImGui выводятся ПОСЛЕ родительского, и
    // картинка сцены накрывает гизмо целиком. Он честно рисовался, просто под
    // изображением. Указатель на ImDrawList живёт до конца кадра, поэтому его
    // можно запомнить здесь и отдать ImGuizmo уже снаружи.
    ImDrawList* slotDrawList[EditorHost::kMaxViews] = {nullptr, nullptr, nullptr, nullptr};

    for (int i = 0; i < viewCount; ++i) {
        if (i > 0 && (viewCount == 2 || i % 2 == 1)) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);
        ImGui::BeginChild("##view", cell, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        slotHovered[i] = ImGui::IsWindowHovered();
        slotPos[i] = ImGui::GetCursorScreenPos();
        slotDrawList[i] = ImGui::GetWindowDrawList();

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

        // --- Приём перетаскивания ассета В СЦЕНУ ----------------------------
        //
        // Тащат туда, где смотрят: модель и префаб встают на поверхность под
        // курсором, материал — на объект, на который его уронили. Раньше
        // вьюпорт перетаскивание не принимал вовсе, и «поставить свою модель»
        // означало создать пустую сущность, найти её в инспекторе и напечатать
        // там путь.
        //
        // Цель обязана сидеть здесь, на item'е картинки: ImGui адресует приём
        // ПОСЛЕДНИМ нарисованным элементом. Сама постановка отложена до места,
        // где посчитаны матрицы активного вида, — бросок в неактивный вид
        // заодно делает его активным, и считать по чужой камере нельзя.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH")) {
                std::string dropped((const char*)p->Data, (size_t)p->DataSize);
                if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
                m_activeSlot = i;
                m_pendingDrop.Active = true;
                m_pendingDrop.Path = dropped;
                m_pendingDrop.Pos = ImGui::GetMousePos();
            }
            ImGui::EndDragDropTarget();
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

    // Дальше — интерактив АКТИВНОГО слота.
    //
    // Гизмо и выбор мышью работают в ЛЮБОМ виде, включая ортогональные: луч
    // строится обратной матрицей вида-проекции, и для ортогональной проекции она
    // ничем не хуже. Раньше весь интерактив выключался вне перспективы — то есть
    // вид сверху и сбоку показывали сцену, но ничего в ней сделать не давали.
    // Полёт камеры остаётся только в перспективе: в ортогональном виде за
    // навигацию отвечают колесо (масштаб) и средняя кнопка (панорама).
    const bool perspective = m_kinds[m_activeSlot] == ViewKind::Perspective;
    const bool hovered = slotHovered[m_activeSlot];
    const ImVec2 imgPos = slotPos[m_activeSlot];
    const ImVec2 avail = cell;
    // Размер буфера главного слота обновляется ВСЕГДА, а не только в перспективе:
    // иначе при смене вида на ортогональный кадр остаётся прежнего размера и
    // растягивается по панели.
    if (m_activeSlot == 0) host.SetViewportSize((int)cell.x, (int)cell.y);

    ImGuiIO& io = ImGui::GetIO();
    float dt = sage::Application::Get().DeltaTime();
    Camera& camera = host.EditorCamera();

    // --- Камера: ПКМ — осмотр, ПКМ+WASDQE — полёт, Shift — ускорение ---
    bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    const bool wasDriving = m_cameraDriving;
    if (perspective && (hovered || m_cameraDriving) && rmb) {
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

    // Курсор ЗАХВАТЫВАЕТСЯ на время осмотра правой кнопкой.
    //
    // Без захвата обзор упирается в край экрана: мышь доезжает до границы
    // монитора и камера просто перестаёт вращаться, хотя кнопка ещё зажата.
    // Развернуться на 360° приходилось в несколько приёмов. В захвате курсор
    // прячется и не имеет границ — движение мыши приходит как чистое смещение,
    // и обзор становится непрерывным.
    //
    // Переключаем ТОЛЬКО на фронте, а не каждый кадр: SetCursorCaptured сам
    // отсекает повтор, но и лишний вызов в кадре тут ни к чему.
    if (m_cameraDriving != wasDriving) {
        sage::Application::Get().GetWindow().SetCursorCaptured(m_cameraDriving);
        // Курсор возвращается ТУДА, ОТКУДА начали смотреть: иначе он всплывает
        // в центре экрана, и следующий клик уходит не по тому объекту.
        if (m_cameraDriving) m_captureReturnPos = ImGui::GetMousePos();
        else ImGui::GetIO().MousePos = m_captureReturnPos;
    }

    if (perspective && hovered && io.MouseWheel != 0.0f) {
        camera.Position += camera.Front * io.MouseWheel * 0.8f;
    }

    // --- Хоткеи гизмо (не во время полёта камеры и не в полях ввода) ---
    if (hovered && !m_cameraDriving && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) host.GizmoOp() = (int)ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) host.GizmoOp() = (int)ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) host.GizmoOp() = (int)ImGuizmo::SCALE;
        if (ImGui::IsKeyPressed(ImGuiKey_T)) host.GizmoOp() = (int)ImGuizmo::UNIVERSAL;
        if (ImGui::IsKeyPressed(ImGuiKey_Y)) host.GizmoOp() = (int)ImGuizmo::BOUNDS;
        // F — показать выделенное в кадре, End — посадить на поверхность.
        // Обе операции до этого делались правкой чисел в инспекторе: подвести
        // камеру к далёкому объекту и посадить его ровно на пол — самые частые
        // и самые муторные действия при сборке сцены.
        if (ImGui::IsKeyPressed(ImGuiKey_F)) host.FocusSelected();
        if (ImGui::IsKeyPressed(ImGuiKey_End)) host.DropSelectedToSurface();
    }

    // --- Режим вёрстки интерфейса -------------------------------------------
    //
    // Идёт ДО 3D-гизмо и вместо него: пока верстают интерфейс, стрелки
    // перемещения объекта в тех же пикселях экрана только мешают, а клик обязан
    // выбирать элемент UI, а не то, что за ним в сцене.
    if (host.UIEditMode()) {
        m_uiCanvas.Draw(host, slotDrawList[m_activeSlot], imgPos, avail, (int)avail.x,
                        (int)avail.y, hovered && perspective);
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // --- ImGuizmo: манипулятор выбранной сущности (сетка — DebugDraw в FBO) ---
    //
    // Матрицы берём У АКТИВНОГО СЛОТА, а не у главного. host.ViewMatrix() — это
    // всегда слот 0; пока активен он, разницы нет, но стоит перейти в вид сверху
    // из раскладки на четыре окна, и гизмо считалось бы по чужой камере — ручки
    // стояли бы не там, где объект.
    const glm::mat4 activeView =
        (m_activeSlot == 0 || !requests[m_activeSlot].Ortho) ? host.ViewMatrix()
                                                             : requests[m_activeSlot].View;
    const glm::mat4 activeProj =
        (m_activeSlot == 0 || !requests[m_activeSlot].Ortho) ? host.ProjMatrix()
                                                             : requests[m_activeSlot].Proj;

    // Ортогональному виду нужна своя математика гизмо: с перспективной
    // ручки в нём тянутся не туда, куда едет объект.
    ImGuizmo::SetOrthographic(!perspective);
    // Список отрисовки — ИМЕННО активного слота (см. slotDrawList выше).
    if (slotDrawList[m_activeSlot]) ImGuizmo::SetDrawlist(slotDrawList[m_activeSlot]);
    else ImGuizmo::SetDrawlist();
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

        const bool rectTool = (ImGuizmo::OPERATION)host.GizmoOp() == ImGuizmo::BOUNDS;

        // Пока гизмо не тащат, но курсор над ним — запоминаем состояние «до»:
        // первый же кадр перетаскивания уже мутирует Transform, поэтому снапшот
        // должен быть взят раньше него.
        //
        // ImGuizmo::IsOver() НЕ ЗНАЕТ про ручки рамки (см. IsOver в ImGuizmo.cpp
        // — там опрошены только translate/rotate/scale), поэтому для рамки
        // снапшот берём по нажатию во вьюпорте. Без этого растягивание рамкой
        // было единственной правкой сцены, которую нельзя отменить.
        if (!ImGuizmo::IsUsing() && !host.InPlayMode() &&
            (ImGuizmo::IsOver() ||
             (rectTool && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
            host.CapturePendingSnapshot();
        }

        // Шаг привязки берём у хоста: он настраивается в тулбаре и запоминается
        // с проектом. Раньше это были три константы, зашитые здесь, — и для
        // постройки из блоков размером в единицу шаг 0.5 означал, что половина
        // построек встаёт со сдвигом на полблока.
        auto op = (ImGuizmo::OPERATION)host.GizmoOp();
        float snapValues[3];
        const float snapUnit = host.SnapStepForCurrentOp();
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

        // --- Rect/Bounds: рамка с ручками по углам габаритной коробки ----------
        //
        // Отдельное гизмо, а не режим масштаба, потому что отвечает на другой
        // вопрос. Scale тянет объект ОТ ЦЕНТРА и одинаково по обе стороны;
        // рамка тянет ОДНУ ГРАНЬ, оставляя противоположную на месте. Для
        // постройки из блоков это основная операция: подогнать стену к проёму
        // масштабом означает попасть одновременно двумя краями, а рамкой —
        // одним.
        //
        // Коробка берётся настоящая (BoundsMin/BoundsMax меша), поэтому ручки
        // сидят на реальных гранях объекта, а не на условном единичном кубе.
        const float* boundsPtr = nullptr;
        const float* boundsSnapPtr = nullptr;
        float localBounds[6] = {-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};
        float boundsSnap[3] = {0.0f, 0.0f, 0.0f};
        if (rectTool) {
            if (const MeshRendererComponent* mr =
                    scene.Registry().try_get<MeshRendererComponent>(selected.Entity())) {
                if (mr->MeshPtr) {
                    const glm::vec3 bmin = mr->MeshPtr->BoundsMin();
                    const glm::vec3 bmax = mr->MeshPtr->BoundsMax();
                    for (int i = 0; i < 3; ++i) {
                        localBounds[i] = bmin[i];
                        localBounds[i + 3] = bmax[i];
                    }
                }
            }
            boundsPtr = localBounds;
            if (host.GizmoSnap()) {
                boundsSnap[0] = boundsSnap[1] = boundsSnap[2] = host.SnapMove();
                boundsSnapPtr = boundsSnap;
            }

            // ОДИН инструмент, а не два поверх друг друга.
            //
            // BOUNDS сам по себе даёт только ручки растягивания, и рамка без
            // переноса была бы ловушкой: объект можно растянуть, но не сдвинуть.
            // Поэтому перенос нужен — но добавленный «как есть», он приносил с
            // собой ВЕСЬ гизмо перемещения: три цветные стрелки и три квадрата
            // плоскостей ровно там же, где рамка. Две системы ручек в одном
            // месте перекрывают друг друга, и попасть по углу рамки, не задев
            // стрелку, было делом везения — рамка выглядела «смешанной» с
            // обычным гизмо и вела себя как он.
            //
            // Маска осей гасит и стрелки, и плоскости (ComputeTripodAxisAnd-
            // Visibility в ImGuizmo проверяет её и для отрисовки, и для попадания),
            // оставляя от переноса РОВНО центральный кружок — перетаскивание в
            // плоскости экрана. Получается инструмент как в любом другом
            // редакторе: рамка тянет грани, центр двигает целиком.
            //
            // Маска глобальна для ImGuizmo, поэтому снимается сразу после
            // Manipulate — иначе следующий кадр с обычным гизмо остался бы без
            // стрелок.
            ImGuizmo::SetAxisMask(true, true, true);
            op = ImGuizmo::BOUNDS | ImGuizmo::TRANSLATE;

            // Рамка считает ручки по СОБСТВЕННЫМ осям объекта (mModelSource в
            // ImGuizmo), поэтому и центр обязан двигаться в том же
            // пространстве: в WORLD повёрнутый объект уезжал бы не туда, куда
            // его тянут. Та же причина, по которой LOCAL принудителен для Scale.
            mode = ImGuizmo::LOCAL;
        }

        const bool manipulated =
            ImGuizmo::Manipulate(glm::value_ptr(activeView), glm::value_ptr(activeProj),
                                 op, mode, glm::value_ptr(model),
                                 nullptr, host.GizmoSnap() ? snapValues : nullptr,
                                 boundsPtr, boundsSnapPtr);

        if (manipulated) {
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

    // Ассет, бро́шенный в этот вид, ставится в сцену (см. приём ниже, у
    // ImGui::Image): матрицы к этому месту уже посчитаны, а сам приём обязан
    // сидеть на item'е картинки — ImGui адресует цель последним нарисованным
    // элементом, и здесь это уже не она.
    if (m_pendingDrop.Active) {
        const float du = (m_pendingDrop.Pos.x - imgPos.x) / avail.x;
        const float dv = (m_pendingDrop.Pos.y - imgPos.y) / avail.y;
        if (du >= 0.0f && du <= 1.0f && dv >= 0.0f && dv <= 1.0f) {
            if (!host.DropAssetAtViewport(activeView, activeProj, du, dv, m_pendingDrop.Path)) {
                host.SetStatusMessage(T("You can drop a model, a prefab or a material into the scene"));
            }
        }
        m_pendingDrop = {};
    }

    // --- Пикинг ЛКМ (не по гизмо и не во время манипуляции) ---
    //
    // Маска осей снимается ПОСЛЕ этой проверки, а не сразу за Manipulate:
    // ImGuizmo::IsOver() опрашивает попадание заново, и по снятой маске он
    // «увидел» бы стрелки перемещения, которых у рамки не нарисовано. Клик
    // рядом с невидимой осью не выбирал бы объект под курсором — тем более
    // странно, что видимой причины для этого на экране нет.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
        ImVec2 mp = ImGui::GetMousePos();
        float u = (mp.x - imgPos.x) / avail.x;
        float v = (mp.y - imgPos.y) / avail.y;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            // Теми же матрицами, что и гизмо: иначе в ортогональном виде или в
            // неглавном слоте луч строился бы по камере другого окна и выбирал
            // бы объекты «не там, куда щёлкнули».
            host.PickAtViewportWith(activeView, activeProj, u, v, io.KeyCtrl);
        }
    }

    // Маска осей глобальна для ImGuizmo — снимаем её здесь, когда все опросы
    // попадания этого кадра уже сделаны. Оставить её значило бы следующий кадр
    // с обычным гизмо без стрелок.
    ImGuizmo::SetAxisMask(false, false, false);

    // Инструменты (режим гизмо, snap, пространство, Play, режим рендера) —
    // в верхнем тулбаре редактора (ToolbarPanel), не оверлеем во вьюпорте.

    ImGui::End(); // Viewport
    ImGui::PopStyleVar();
}

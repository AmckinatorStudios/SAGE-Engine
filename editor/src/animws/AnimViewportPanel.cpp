#include "AnimPanels.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "ImGuizmo.h"

#include "AnimStage.h"
#include "../CursorCapture.h"
#include "../EditorHost.h"
#include "../EditorPrefs.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../InspectorControls.h"
#include "../Localization.h"
#include "sage/anim/AnimationComponents.h"
#include "sage/scene/Components.h"

namespace ictl = sage::editor::ictl;

namespace {

// Точка мира -> точка экрана. false — за камерой.
bool ProjectPoint(const glm::mat4& viewProj, const glm::vec3& p, const glm::vec2& origin, const glm::vec2& size,
             glm::vec2& out) {
    const glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
    if (c.w <= 1e-4f) return false;
    const glm::vec3 ndc = glm::vec3(c) / c.w;
    out = origin + glm::vec2((ndc.x * 0.5f + 0.5f) * size.x, (1.0f - (ndc.y * 0.5f + 0.5f)) * size.y);
    return true;
}

float SegmentDistance(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    const float t = len2 > 1e-6f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

} // namespace

void AnimViewportPanel::DrawToolbar(EditorHost& host, AnimStage& stage) {
    (void)host;
    // Инструменты — как у вьюпорта сцены: те же значки и клавиши Q / W / E / R.
    // Q — «только выбор»: гизмо скрыто, выбор костей работает.
    static const int kOrder[4] = {SelectOnly, Move, Rotate, Scale};
    ictl::GroupItem ops[4];
    ops[0].Icon = "select";
    ops[0].Tooltip = T("Select only (Q): the gizmo is hidden, bones are still picked");
    ops[1].Icon = "move";
    ops[1].Tooltip = T("Move (W)");
    ops[2].Icon = "rotate";
    ops[2].Tooltip = T("Rotate (E)");
    ops[3].Icon = "scale";
    ops[3].Tooltip = T("Scale (R)");
    for (int i = 0; i < 4; ++i) ops[i].Active = m_op == kOrder[i];
    if (const int hit = ictl::ButtonGroup("##anim_ops", ops, 4); hit >= 0) m_op = kOrder[hit];
    ImGui::SameLine();
    ictl::GroupItem space[2];
    space[0].Text = T("Local");
    space[0].Tooltip = T("Gizmo axes of the bone or object itself");
    space[0].Active = m_local;
    space[1].Text = T("World");
    space[1].Tooltip = T("Gizmo axes of the world");
    space[1].Active = !m_local;
    // Масштаб — всегда в своих осях: в мировых он перекашивал бы кость.
    if (const int hit = ictl::ButtonGroup("##anim_space", space, 2, ictl::ButtonLook::Standard); hit >= 0)
        m_local = hit == 0;
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("fit", T("Frame Target (F)"))) stage.FrameTarget();
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("refresh", T("Reset the camera"))) stage.ResetCamera();
    ImGui::SameLine();
    // Две независимые настройки вида. Скелет не связан с выбором: кость
    // выбирается и в Animation Hierarchy, когда он скрыт.
    if (ictl::Button(m_showSkeleton ? "eye" : "eye-off", T("Skeleton"), T("Show Skeleton: lines and points of all bones"),
                     ictl::ButtonLook::Standard, 0.0f, m_showSkeleton)) {
        m_showSkeleton = !m_showSkeleton;
        sage::editor::prefs::SetBool("anim.showSkeleton", m_showSkeleton);
    }
    ImGui::SameLine();
    if (ictl::Button("focus", T("Follow"), T("Follow Target: the camera follows the model while it plays"),
                     ictl::ButtonLook::Standard, 0.0f, stage.Follow())) {
        stage.SetFollow(!stage.Follow());
        sage::editor::prefs::SetBool("anim.follow", stage.Follow());
    }
    if (stage.HasPoseEdits()) {
        ImGui::SameLine();
        if (ictl::Button("undo", T("Reset pose"), T("Drop the pose edits that are not keyed yet")))
            stage.ClearPose();
    }
}

// Глубина точки вдоль взгляда камеры.
static float ViewDepth(const AnimStage& stage, const glm::vec3& p) {
    const glm::mat4 v = stage.View();
    return -(v * glm::vec4(p, 1.0f)).z;
}

void AnimViewportPanel::DrawBones(AnimStage& stage, const glm::mat4& viewProj, const glm::vec2& origin,
                                  const glm::vec2& size) {
    const sage::anim::Skeleton* sk = stage.Skeleton();
    if (!sk) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 picked = EditorTheme::Color32(EditorTheme::Role::Accent);
    const int sel = stage.SelectedBone();
    // ГЛУБИНА. Кости лежат внутри тела и видны поверх него, а за ним —
    // те же линии, что спереди, и они путают, что именно под курсором. Кости
    // дальше фокусной плоскости на размер модели бледнеют до трети: передние
    // читаются первыми. Настоящего перекрытия геометрией здесь нет — у
    // предпросмотра нет чтения глубины, — поэтому это затухание по глубине, а
    // не скрытие; полностью убрать скелет можно кнопкой Skeleton.
    const float focusDepth = stage.Distance();
    const float span = std::max(0.05f, stage.Radius());
    auto fade = [&](const glm::vec3& p) {
        const float behind = std::clamp((ViewDepth(stage, p) - focusDepth) / span, 0.0f, 1.0f);
        return 1.0f - 0.65f * behind;
    };
    auto withAlpha = [](ImU32 col, float k) {
        const ImU32 a = (ImU32)((float)((col >> IM_COL32_A_SHIFT) & 0xFF) * k);
        return (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    };
    const ImU32 bone = IM_COL32(40, 120, 220, 230);
    // Кость — отрезок от родителя к суставу, сустав — кружок. Выбранная — цветом
    // акцента вместе с отрезком к ней: видно, ЧТО повернётся. Выбранная
    // рисуется и при скрытом скелете: иначе в режиме «только выбор» не видно,
    // что выбрано.
    for (int i = 0; i < sk->Count(); ++i) {
        if (!m_showSkeleton && i != sel) continue;
        const glm::vec3 pw = glm::vec3(stage.BoneWorld(i)[3]);
        glm::vec2 a, b;
        if (!ProjectPoint(viewProj, pw, origin, size, b)) continue;
        const int parent = sk->Joints[(size_t)i].Parent;
        const float k = i == sel ? 1.0f : fade(pw);
        const ImU32 col = withAlpha(i == sel ? picked : bone, k);
        if (parent >= 0 && ProjectPoint(viewProj, glm::vec3(stage.BoneWorld(parent)[3]), origin, size, a))
            dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), col, i == sel ? 3.0f : 2.0f);
        dl->AddCircleFilled(ImVec2(b.x, b.y), i == sel ? 5.0f : 3.5f, col);
    }
}

int AnimViewportPanel::PickBone(AnimStage& stage, const glm::mat4& viewProj, const glm::vec2& origin,
                                const glm::vec2& size, const glm::vec2& mouse) const {
    const sage::anim::Skeleton* sk = stage.Skeleton();
    if (!sk) return -1;
    // Сначала сустав (точнее), затем отрезок кости (длинную кость берут за
    // середину). Отрезок отдаёт ДОЧЕРНИЙ сустав: кость поворачивается в нём.
    //
    // ГЛУБИНА: из кандидатов под курсором выигрывает не только ближайший по
    // пикселям, но и ближайший к камере — кость спереди перекрывает ту, что
    // за телом на одной линии взгляда. Штраф глубины мал (до ~двух пикселей на
    // радиус модели), чтобы точный щелчок по дальней кости всё же попадал.
    const float span = std::max(0.05f, stage.Radius());
    float minDepth = 1e30f;
    for (int i = 0; i < sk->Count(); ++i)
        minDepth = std::min(minDepth, ViewDepth(stage, glm::vec3(stage.BoneWorld(i)[3])));
    int best = -1;
    float bestScore = 1e30f;
    for (int i = 0; i < sk->Count(); ++i) {
        const glm::vec3 pw = glm::vec3(stage.BoneWorld(i)[3]);
        glm::vec2 p;
        if (!ProjectPoint(viewProj, pw, origin, size, p)) continue;
        const float d = glm::length(p - mouse);
        if (d >= 9.0f) continue;
        const float score = d + 2.0f * (ViewDepth(stage, pw) - minDepth) / span;
        if (score < bestScore) { bestScore = score; best = i; }
    }
    if (best >= 0) return best;
    bestScore = 1e30f;
    for (int i = 0; i < sk->Count(); ++i) {
        const int parent = sk->Joints[(size_t)i].Parent;
        const glm::vec3 pw = glm::vec3(stage.BoneWorld(i)[3]);
        glm::vec2 a, b;
        if (parent < 0 || !ProjectPoint(viewProj, glm::vec3(stage.BoneWorld(parent)[3]), origin, size, a) ||
            !ProjectPoint(viewProj, pw, origin, size, b))
            continue;
        const float d = SegmentDistance(mouse, a, b);
        if (d >= 6.0f) continue;
        const float score = d + 2.0f * (ViewDepth(stage, pw) - minDepth) / span;
        if (score < bestScore) { bestScore = score; best = i; }
    }
    return best;
}

void AnimViewportPanel::Manipulate(EditorHost& host, AnimStage& stage, const glm::vec2& origin, const glm::vec2& size) {
    (void)host;
    const int bone = stage.SelectedBone();
    const entt::entity ent = stage.SelectedEntity();
    Scene& scene = stage.StageScene();
    const bool hasEntity = ent != entt::null && scene.Registry().valid(ent);
    // «Только выбор» — ни ручек на экране, ни перехвата кликов гизмо.
    if ((bone < 0 && !hasEntity) || m_op == SelectOnly) {
        m_gizmoWasUsing = false;
        return;
    }
    glm::mat4 model = bone >= 0 ? stage.BoneWorld(bone) : scene.WorldMatrix(ent);
    const glm::mat4 view = stage.View();
    const glm::mat4 proj = stage.Proj(size.x / std::max(1.0f, size.y));
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    const ImGuizmo::OPERATION op = m_op == Move ? ImGuizmo::TRANSLATE : m_op == Rotate ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
    const ImGuizmo::MODE mode = (m_local || m_op == Scale) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, mode, glm::value_ptr(model))) {
        if (bone >= 0) {
            stage.SetBoneWorld(bone, model, m_op == Move, m_op == Rotate, m_op == Scale);
        } else if (Transform* tr = scene.Registry().try_get<Transform>(ent)) {
            // Объект копии: локальный трансформ относительно родителя. Уровень
            // не трогается — ключ на это значение ставит таймлайн.
            const entt::entity p = scene.ParentOf(ent);
            tr->SetFromMatrix(p != entt::null ? glm::inverse(scene.WorldMatrix(p)) * model : model);
            stage.NotePoseEdit();
        }
    }
    m_gizmoWasUsing = ImGuizmo::IsUsing();
}

void AnimViewportPanel::ReleaseCapture() {
    m_flying = m_orbiting = false;
    if (!m_captured) return;
    m_captured = false;
    cursorcapture::CaptureForCurrentWindow(false);
    // Курсор возвращается ТУДА, ОТКУДА начали жест, а не в центр окна.
    ImGui::GetIO().MousePos = ImVec2(m_returnX, m_returnY);
}

// КАМЕРА. ПКМ — осмотр на месте, с WASD / Q / E — полёт, Shift — быстрее;
// средняя кнопка — орбита вокруг фокуса, с Shift — сдвиг; колесо — расстояние.
// Жест начинается только над видом и живёт, пока кнопка зажата, даже когда
// курсор ушёл за край окна: на его время курсор захватывается (как в сцене),
// и движение мыши не упирается в край экрана.
void AnimViewportPanel::UpdateCamera(AnimStage& stage, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 look = LookMouseDelta();   // в захвате io.MouseDelta — ноль (см. CursorCapture.h)
    const bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    const bool mmb = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    // Окно потеряло фокус посреди жеста — жест кончился.
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (!m_flying && !m_orbiting && hovered && !ImGuizmo::IsUsing()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) m_flying = true;
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) m_orbiting = true;
    }
    if (m_flying && (!rmb || !focused)) m_flying = false;
    if (m_orbiting && (!mmb || !focused)) m_orbiting = false;

    if (m_flying) {
        stage.Look(look.x, look.y);
        if (!io.WantTextInput) {
            auto axis = [](ImGuiKey plus, ImGuiKey minus) {
                return (ImGui::IsKeyDown(plus) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(minus) ? 1.0f : 0.0f);
            };
            const float f = axis(ImGuiKey_W, ImGuiKey_S), r = axis(ImGuiKey_D, ImGuiKey_A),
                        u = axis(ImGuiKey_E, ImGuiKey_Q);
            if (f != 0.0f || r != 0.0f || u != 0.0f) stage.Fly(f, r, u, io.DeltaTime, io.KeyShift);
        }
    } else if (m_orbiting) {
        if (io.KeyShift) stage.Pan(look.x, look.y);
        else stage.Orbit(-look.x * 0.4f, look.y * 0.4f);
    }

    const bool held = m_flying || m_orbiting;
    if (held && !m_captured) {
        m_captured = true;
        m_returnX = io.MousePos.x;
        m_returnY = io.MousePos.y;
        cursorcapture::CaptureForCurrentWindow(true);
    } else if (!held && m_captured) {
        ReleaseCapture();
    }
    if (hovered && !held && io.MouseWheel != 0.0f) stage.Zoom(io.MouseWheel);
}

void AnimViewportPanel::Draw(EditorHost& host, bool* open, const std::string& windowId) {
    if (!*open) {
        ReleaseCapture();
        return;
    }
    const std::string title = std::string(T("Viewport")) + "###" + windowId;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    const bool visible = ImGui::Begin(title.c_str(), open);
    ImGui::PopStyleVar();
    AnimStage* stagePtr = host.AnimationStage();
    if (!visible || !stagePtr) {
        ReleaseCapture();
        ImGui::End();
        return;
    }
    AnimStage& stage = *stagePtr;
    if (!m_prefsLoaded) {
        m_prefsLoaded = true;
        m_showSkeleton = sage::editor::prefs::GetBool("anim.showSkeleton", true);
        stage.SetFollow(sage::editor::prefs::GetBool("anim.follow", false));
    }
    DrawToolbar(host, stage);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const glm::vec2 size(std::max(32.0f, avail.x), std::max(32.0f, avail.y));
    // ВИД — ДОЧЕРНЕЕ ОКНО, а не кнопка поверх картинки. ImGuizmo берёт ручку
    // только когда ни один элемент ImGui не наведён (IsAnyItemHovered), а
    // невидимая кнопка на весь вид наведена всегда — гизмо не хватало ни одной
    // ручки, и кости нельзя было ни двигать, ни вращать. У сцены вид так же
    // дочернее окно: оно не элемент, и ручки свободны.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##anim_view", ImVec2(size.x, size.y), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const glm::vec2 origin(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y);
    const float scale = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
    const uint64_t tex = stage.Render((int)(size.x * scale), (int)(size.y * scale));
    if (tex) ImGui::Image((ImTextureID)(std::intptr_t)tex, ImVec2(size.x, size.y), ImVec2(0, 1), ImVec2(1, 0));
    else ImGui::Dummy(ImVec2(size.x, size.y));

    const glm::mat4 viewProj = stage.Proj(size.x / size.y) * stage.View();
    DrawBones(stage, viewProj, origin, size);

    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered();
    UpdateCamera(stage, hovered);
    const bool camHeld = m_flying || m_orbiting;
    // Гизмо слушает мышь только над видом и не во время жеста камеры.
    ImGuizmo::Enable(hovered && !camHeld);
    Manipulate(host, stage, origin, size);
    ImGuizmo::Enable(true);
    const bool gizmoHot = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

    // Выбор — щелчок левой без протяжки и не по ручке гизмо. Мимо костей — снять.
    if (hovered && !camHeld && !gizmoHot && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        io.MouseDragMaxDistanceSqr[0] < io.MouseDragThreshold * io.MouseDragThreshold && !m_gizmoWasUsing) {
        const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
        if (stage.Skeletal()) {
            // Скрытый скелет не выбирается щелчком (нечего целить); кость
            // выбирают в Animation Hierarchy, и она остаётся видимой.
            if (m_showSkeleton) stage.SelectBone(PickBone(stage, viewProj, origin, size, mouse));
        } else {
            // Объект копии — ближайшее начало координат под курсором.
            Scene& scene = stage.StageScene();
            entt::entity best = entt::null;
            float bestDist = 14.0f;
            for (auto e : scene.Registry().view<Transform>()) {
                if (GameObject(&scene.Registry(), e).Name().rfind("##", 0) == 0) continue;   // пол
                glm::vec2 p;
                if (!ProjectPoint(viewProj, glm::vec3(scene.WorldMatrix(e)[3]), origin, size, p)) continue;
                const float d = glm::length(p - mouse);
                if (d < bestDist) { bestDist = d; best = e; }
            }
            stage.SelectEntity(best);
        }
    }
    // Клавиши инструментов — пока курсор над видом и не набирают текст. Во время
    // жеста камеры они заняты полётом: W / E / Q там двигают камеру, а не
    // переключают гизмо.
    if ((hovered || ImGui::IsWindowFocused()) && !io.WantTextInput && !camHeld) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) m_op = SelectOnly;
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_op = Move;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_op = Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_op = Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) stage.FrameTarget();
    }

    if (stage.TargetId() < 0) {
        const char* hint = T("Choose a model or object in Animation Inspector > Target");
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + (size.x - ts.x) * 0.5f, origin.y + size.y * 0.5f),
                                            IM_COL32(40, 40, 46, 255), hint);
    }
    ImGui::EndChild();
    ImGui::End();
}

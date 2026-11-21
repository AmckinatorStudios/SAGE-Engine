#include "TopBarPanel.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "imgui.h"

#include "EditorHost.h"
#include "Project.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "ui/UI.h"
#include "../Localization.h"

using sage::editor::T;
using EditorTheme::Role;

// ---------------------------------------------------------------------------
//  ВЕРХНЯЯ ПАНЕЛЬ — ТРИ ГРУППЫ, ШИРИНА СЧИТАЕТСЯ ПО ФАКТУ.
//
//  ЧТО БЫЛО НЕ ТАК. Слоты под запуск и правый блок имели ширину
//  ControlHeight * 8 и * 7 — числа, не связанные с настоящей шириной кнопок.
//  Добавишь кнопку — слот не растёт, и всё лезет друг на друга; уберёшь —
//  остаётся дыра. Курсор при этом двигался через SameLine, и любая ошибка в
//  ширине одной кнопки сдвигала все следующие.
//
//  ЧТО СТАЛО. Каждая группа описана ОДИН раз функцией, которая вызывается
//  дважды: «вхолостую» (измерить ширину) и «по-настоящему» (нарисовать). Ширина
//  и рисунок не могут разойтись — это один и тот же код. Новая кнопка
//  добавляется в функцию своей группы одной строкой, пересчёт не нужен.
//
//  Группы: слева — файл, пространства, отмена; по центру — запуск; справа —
//  настройки и сборка. Когда места мало, берётся первый подходящий уровень из
//  kLevels: сначала уже промежутки, потом подписи пропадают, потом второстепенное
//  уходит в меню «Ещё». Play, настройки и сборка остаются на панели всегда.
// ---------------------------------------------------------------------------
namespace {

// Уровень сжатия. Идёт от просторного к тесному; берётся первый, куда всё влезает.
struct Level {
    bool tight;          // уменьшенные промежутки
    bool workspaceLabels;
    bool rightLabels;    // подписи у «Настройки» и «Собрать»
    bool playLabel;
    bool fileInMenu;     // новая/открыть/проект и отмена — в меню «Ещё»
    bool workspaceInMenu;
};
constexpr Level kLevels[] = {
    {false, true,  true,  true,  false, false},
    {true,  true,  true,  true,  false, false},
    {true,  false, true,  true,  false, false},
    {true,  false, false, true,  false, false},
    {true,  false, false, false, false, false},
    {true,  false, false, false, true,  false},
    {true,  false, false, false, true,  true},
};

// Курсор группы. Позиции задаются явно (SetCursorPos), а не через SameLine:
// ошибка в ширине одного элемента не сдвигает остальные.
struct Pen {
    bool draw = false;
    float x = 0.0f;       // позиция относительно начала группы
    float originX = 0.0f; // где группа начинается в окне
    float top = 0.0f;
    float gap = 0.0f;

    // Разместить элемент шириной w; fn рисует его (при измерении не вызывается).
    template <class Fn> void Item(float w, Fn&& fn) {
        if (draw) {
            ImGui::SetCursorPos(ImVec2(std::floor(originX + x), top));
            fn();
        }
        x += w + gap;
    }
    float Width() const { return std::max(0.0f, x - gap); }
};

struct Metrics {
    float h, padX, iconS;
    float IconW() const { return h; }
    // Ширина кнопки «значок + подпись» — та же формула, что в EditorIcons::Button.
    float LabelW(const char* label) const {
        return EditorIcons::LabeledWidth(iconS, label) + padX * 2.0f;
    }
};

} // namespace

void TopBarPanel::Draw(EditorHost& host, float height) {
    const Sage::UI::Style& ui = Sage::UI::Get();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui.PaddingControl, ui.PaddingControlY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(Role::Bg));
    ImGui::BeginChild("##topbar", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float windowW = ImGui::GetWindowWidth();
    const ImVec2 barMin = ImGui::GetWindowPos();

    Metrics m;
    m.h = ImGui::GetFrameHeight();
    m.padX = ImGui::GetStyle().FramePadding.x;
    m.iconS = std::floor(m.h * 0.68f);
    const float top = std::floor((height - m.h) * 0.5f);   // единая вертикаль всех кнопок

    const EditorPlayState state = host.GetPlayState();
    const bool playing = state == EditorPlayState::Playing;
    const bool paused = state == EditorPlayState::Paused;
    const bool inPlay = host.InPlayMode();
    const EditorWorkspace space = host.Workspace();
    bool openMore = false;

    // ---- ЛЕВАЯ ГРУППА -------------------------------------------------------
    auto left = [&](Pen& pen, const Level& lv) {
        const float groupGap = lv.tight ? ui.SpacingMD : ui.SpacingMD * 2.0f;
        auto divider = [&]() {
            pen.x = pen.x - pen.gap + groupGap * 0.5f;
            if (pen.draw) {
                const float dx = std::floor(barMin.x + pen.originX + pen.x);
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(dx, barMin.y + ui.SpacingSM), ImVec2(dx, barMin.y + height - ui.SpacingSM),
                    ImGui::GetColorU32(EditorTheme::Color(Role::Line)), ui.BorderWidth);
            }
            pen.x += groupGap * 0.5f;
        };
        if (!lv.fileInMenu) {
            pen.Item(m.IconW(), [&] { if (EditorIcons::IconOnlyButton("file", T("New scene"))) host.NewSceneWithPrompt(); });
            pen.Item(m.IconW(), [&] { if (EditorIcons::IconOnlyButton("open", T("Open scene..."))) host.RequestDialog("Open Scene"); });
            pen.Item(m.IconW(), [&] { if (EditorIcons::IconOnlyButton("project", T("Open project..."))) host.RequestDialog("Open Project"); });
        }
        pen.Item(m.IconW(), [&] { if (EditorIcons::IconOnlyButton("save", T("Save scene (Ctrl+S)"))) host.SaveCurrentScene(); });

        if (!lv.workspaceInMenu) {
            divider();
            // Переключатели, а не меню: пространства меняют часто.
            const struct { EditorWorkspace ws; const char* icon; const char* label; const char* tip; } kSpaces[] = {
                {EditorWorkspace::Scene, "cube", "Scene", "Place objects in the world"},
                {EditorWorkspace::Interface, "layout", "Interface", "Design the game interface"},
                {EditorWorkspace::Animation, "anim", "Animation", "Animate skeletons: clips and animation graphs"},
            };
            for (const auto& sp : kSpaces) {
                const float w = lv.workspaceLabels ? m.LabelW(T(sp.label)) : m.IconW();
                pen.Item(w, [&] {
                    const bool on = space == sp.ws;
                    const bool pressed = lv.workspaceLabels
                        ? EditorIcons::Button(sp.icon, T(sp.label), T(sp.tip), on)
                        : EditorIcons::IconOnlyButton(sp.icon, T(sp.label), on);
                    if (pressed) host.SetWorkspace(sp.ws);
                });
            }
        }

        if (!lv.fileInMenu) {
            divider();
            // Гаснут, когда отменять нечего: живая кнопка без действия читается как поломка.
            const bool canUndo = host.History().CanUndo() && !inPlay;
            const bool canRedo = host.History().CanRedo() && !inPlay;
            pen.Item(m.IconW(), [&] {
                ImGui::BeginDisabled(!canUndo);
                if (EditorIcons::IconOnlyButton("undo", T("Undo (Ctrl+Z)"))) host.Undo();
                ImGui::EndDisabled();
            });
            pen.Item(m.IconW(), [&] {
                ImGui::BeginDisabled(!canRedo);
                if (EditorIcons::IconOnlyButton("redo", T("Redo (Ctrl+Y)"))) host.Redo();
                ImGui::EndDisabled();
            });
        }
        if (lv.fileInMenu || lv.workspaceInMenu) {
            divider();
            pen.Item(m.IconW(), [&] { if (EditorIcons::IconOnlyButton("dots", T("More"))) openMore = true; });
        }
    };

    // ---- ЦЕНТР: ЗАПУСК ------------------------------------------------------
    //
    // Слот резервируется под ВСЕ состояния: кнопка «Play»/«Resume» занимает
    // ширину более длинной из подписей, а пауза/шаг/стоп стоят сразу за ней. При
    // переходе Edit → Playing → Paused ничего не сдвигается.
    auto center = [&](Pen& pen, const Level& lv) {
        const float playW = lv.playLabel ? std::max(m.LabelW(T("Play")), m.LabelW(T("Resume")))
                                         : m.IconW();
        pen.Item(playW, [&] {
            // Единственное, что красится акцентом: ради него жёлтый и держат в резерве.
            ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(Role::Accent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Color(Role::AccentHover));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Color(Role::AccentActive));
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextOnAccent));
            const char* icon = "play";
            const char* label = paused ? T("Resume") : T("Play");
            const char* tip = paused ? T("Resume") : T("Run the scene (it is restored on Stop)");
            const bool pressed = lv.playLabel ? EditorIcons::Button(icon, label, tip, playing)
                                              : EditorIcons::IconOnlyButton(icon, tip, playing);
            if (pressed) {
                if (paused) host.ResumePlay();
                else if (!playing) host.StartPlay();
            }
            ImGui::PopStyleColor(4);
        });
        pen.Item(m.IconW(), [&] {
            ImGui::BeginDisabled(!playing);
            if (EditorIcons::IconOnlyButton("pause", T("Pause"))) host.PausePlay();
            ImGui::EndDisabled();
        });
        pen.Item(m.IconW(), [&] {   // шаг — только на паузе
            ImGui::BeginDisabled(!paused);
            if (EditorIcons::IconOnlyButton("step", T("One frame forward (only while paused)"))) host.StepPlay();
            ImGui::EndDisabled();
        });
        pen.Item(m.IconW(), [&] {
            ImGui::BeginDisabled(!inPlay);
            if (EditorIcons::IconOnlyButton("stop", T("Stop and restore the scene"))) host.StopPlay();
            ImGui::EndDisabled();
        });
    };

    // ---- ПРАВАЯ ГРУППА: НАСТРОЙКИ И СБОРКА -----------------------------------
    //
    // Пара одинаковой высоты и устройства: оба — «значок + подпись» (или оба
    // только значки в тесном окне). Привязка гизмо здесь не нужна: она живёт во
    // вьюпорте рядом с жестом (ViewportTools.cpp).
    auto right = [&](Pen& pen, const Level& lv) {
        const char* gs = T("Settings");
        const char* bd = T("Build");
        pen.Item(lv.rightLabels ? m.LabelW(gs) : m.IconW(), [&] {
            bool& open = host.PanelVisible(EditorPanel::Settings);
            const bool pressed = lv.rightLabels
                ? EditorIcons::Button("gear", gs, T("Game Settings"), open)
                : EditorIcons::IconOnlyButton("gear", T("Game Settings"), open);
            if (pressed) open = !open;
        });
        pen.Item(lv.rightLabels ? m.LabelW(bd) : m.IconW(), [&] {
            ImGui::BeginDisabled(inPlay);
            const char* tip = T("Collects the game into a folder you can hand over:\n"
                                "the player, the project and the assets it uses");
            const bool pressed = lv.rightLabels ? EditorIcons::Button("build", bd, tip)
                                                : EditorIcons::IconOnlyButton("build", tip);
            if (pressed) host.RequestDialog("Build Game");
            ImGui::EndDisabled();
        });
    };

    // ---- ВЫБОР УРОВНЯ -------------------------------------------------------
    const float pad = ui.PaddingPanel;
    const Level* lvp = &kLevels[std::size(kLevels) - 1];
    float lw = 0, cw = 0, rw = 0, itemGap = 0;
    for (const Level& lv : kLevels) {
        const float g = lv.tight ? 2.0f : ui.SpacingXS;
        const float groupGap = lv.tight ? ui.SpacingMD : ui.SpacingMD * 2.0f;
        Pen a, b, c;
        a.gap = b.gap = c.gap = g;
        left(a, lv); center(b, lv); right(c, lv);
        lw = a.Width(); cw = b.Width(); rw = c.Width(); itemGap = g;
        lvp = &lv;
        if (pad * 2.0f + lw + cw + rw + groupGap * 2.0f <= windowW) break;
    }
    const Level& lv = *lvp;
    const float groupGap = lv.tight ? ui.SpacingMD : ui.SpacingMD * 2.0f;

    // Центр — по центру ОКНА (положение Play не зависит от того, сколько кнопок
    // слева и справа), но не заходя в соседние группы.
    const float leftEnd = pad + lw;
    const float rightStart = windowW - pad - rw;
    float cx = std::floor((windowW - cw) * 0.5f);
    cx = std::min(cx, rightStart - groupGap - cw);
    cx = std::max(cx, leftEnd + groupGap);

    auto run = [&](auto&& group, float originX) {
        Pen pen;
        pen.draw = true; pen.gap = itemGap; pen.originX = originX; pen.top = top;
        group(pen, lv);
    };
    run(left, pad);
    run(center, cx);
    run(right, rightStart);

    // Разделители между левой/правой группой и центром — по середине зазора.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto sep = [&](float x) {
        x = std::floor(barMin.x + x);
        dl->AddLine(ImVec2(x, barMin.y + ui.SpacingSM), ImVec2(x, barMin.y + height - ui.SpacingSM),
                    ImGui::GetColorU32(EditorTheme::Color(Role::Line)), ui.BorderWidth);
    };
    sep((leftEnd + cx) * 0.5f);
    sep((cx + cw + rightStart) * 0.5f);

    // ---- МЕНЮ «ЕЩЁ»: то, что не влезло ---------------------------------------
    if (openMore) ImGui::OpenPopup("TopBarMore###TopBarMore");
    if (Sage::UI::MenuScope menu; ImGui::BeginPopup("TopBarMore###TopBarMore")) {
        if (lv.fileInMenu) {
            if (EditorIcons::MenuItem("file", T("New scene"))) host.NewSceneWithPrompt();
            if (EditorIcons::MenuItem("open", T("Open scene..."))) host.RequestDialog("Open Scene");
            if (EditorIcons::MenuItem("project", T("Open project..."))) host.RequestDialog("Open Project");
            ImGui::Separator();
            if (EditorIcons::MenuItem("undo", T("Undo (Ctrl+Z)"), nullptr,
                                      host.History().CanUndo() && !inPlay)) host.Undo();
            if (EditorIcons::MenuItem("redo", T("Redo (Ctrl+Y)"), nullptr,
                                      host.History().CanRedo() && !inPlay)) host.Redo();
        }
        if (lv.workspaceInMenu) {
            if (lv.fileInMenu) ImGui::Separator();
            if (EditorIcons::MenuItemSelected("cube", T("Scene"), space == EditorWorkspace::Scene))
                host.SetWorkspace(EditorWorkspace::Scene);
            if (EditorIcons::MenuItemSelected("layout", T("Interface"), space == EditorWorkspace::Interface))
                host.SetWorkspace(EditorWorkspace::Interface);
            if (EditorIcons::MenuItemSelected("anim", T("Animation"), space == EditorWorkspace::Animation))
                host.SetWorkspace(EditorWorkspace::Animation);
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

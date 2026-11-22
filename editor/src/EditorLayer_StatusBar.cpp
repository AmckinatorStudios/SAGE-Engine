// ---------------------------------------------------------------------------
// EditorLayer — каркас окна: докинг, меню, строка состояния.
//
// Рама вокруг панелей: пространство докинга, главное меню, строка состояния,
// окно «о программе» и подсказка на пустом доке. Это не редактор как таковой, а
// его оболочка — она не знает ни про сцену, ни про ассеты, и меняется по совсем
// другим поводам.
//
// Часть класса EditorLayer: объявления методов остались в EditorLayer.h, здесь
// только тела. Разбит он ровно потому, что дорос до двух с половиной тысяч
// строк, в которых рядом лежали сборка игры, отмена правки и раскладка окон —
// три области, у которых нет ничего общего, кроме имени класса.
// ---------------------------------------------------------------------------
#include "CreateAssetDialog.h"
#include "EditorLayer.h"
#include "LuaPlugins.h"
#include "PluginsWindow.h"
#include "FileBrowser.h"
#include "ModelImportDialog.h"
#include "sage/assets/Pack.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <fstream>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API (создание раскладки по умолчанию)
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"

#include "CodeEditorApp.h"
#include "EditorTheme.h"
#include "ui/UI.h"
#include "sage/core/Profiler.h"
#include "EditorIcons.h"
#include "HotkeyScope.h"
#include "ModelMaterialImport.h"
#include "sage/render/DebugView.h"
#include "sage/app/Application.h"
#include "sage/core/Paths.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Systems.h"
#include "sage/core/Version.h"
#include "sage/core/CrashHandler.h"
#include "sage/render/MeshRaycast.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/render/LightingUpload.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/gi/GI.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/SceneSerializer.h"
#include "Localization.h"
#include "ObjectCatalog.h"
#include "PanelWindowId.h"

namespace fs = std::filesystem;

// Строка состояния, окно «О программе» и подсказка пустого дока.
// Help > About SAGE — КОРОТКО: что это за программа, какая версия и как
// скопировать её для сообщения об ошибке.
//
// Здесь была таблица из трёх десятков подсистем с версиями «v1» у каждой —
// на весь экран и без ответа на вопрос, зачем окно открыли. Открывают его за
// двумя вещами: узнать версию и передать её вместе с сообщением о проблеме.
// Список подсистем остался — свёрнутым, для того самого сообщения.
void EditorLayer::DrawAboutWindow() {
    if (!m_showAbout) return;
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin(T("About SAGE" "###About SAGE"), &m_showAbout,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::Text(T("SAGE Engine %s"), kSageEngineVersion);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420.0f);
    ImGui::TextDisabled("%s", T("An engine and editor for making games: scenes, 3D, physics, sound, "
                                "interface and Lua scripts."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const auto& systems = sage::EngineSystems();
    // Строка для сообщения об ошибке: версия и набор подсистем — ровно то, что
    // спросят первым делом.
    if (ImGui::Button(T("Copy version for a bug report"))) {
        std::string text = std::string("SAGE Engine ") + kSageEngineVersion + "\n";
        for (const sage::SystemVersion& sys : systems) text += std::string(sys.Name) + " " + sys.Tag() + "\n";
        ImGui::SetClipboardText(text.c_str());
        SetStatusMessage(T("Version copied"));
    }

    if (ImGui::CollapsingHeader(T("Subsystems"))) {
        if (ImGui::BeginTable("##systems", 2,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_ScrollY,
                              ImVec2(420.0f, 220.0f))) {
            ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch);
            for (const sage::SystemVersion& sys : systems) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sys.Name);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", sys.Summary);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}


// СТРОКА СОСТОЯНИЯ — коротко и по делу.
//
// Слева — ГДЕ Я: проект (приглушённо), сцена (читаемо) и точка «есть
// несохранённое»; за ними — ЧТО СЛОМАНО: счётчики ошибок (красным) и
// предупреждений (приглушённо-жёлтым), только у тех, что не ноль, каждый —
// кнопка в консоль с отбором этого уровня; дальше — Play/Пауза и сообщения
// инструментов. Справа — только FPS.
//
// ЧЕГО НЕТ И ПОЧЕМУ. Сущности, память, вызовы отрисовки, треугольники и
// объекты стояли здесь постоянно, а смотрят на них раз в день — их строка
// читалась как таблица, и глаз не находил главного: есть ли ошибки. Они
// остались в информации вьюпорта («…» → «Информация во вьюпорте»), версия
// движка — в окне «О программе». Разделителей «|» нет: группы разделяет
// воздух. Декоративных значков нет: значок есть только у ошибок и
// предупреждений, где он отличает одно от другого.
//
// УЗКОЕ ОКНО. Части включаются по важности — сцена, проблемы, Play, проект,
// FPS, сообщения — пока помещаются; лишнее не рисуется, а не наезжает.
void EditorLayer::DrawStatusBar(float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##statusbar", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const Sage::UI::Style& ui = Sage::UI::Get();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth();
    const float pad = ui.SpacingSM, gap = ui.SpacingLG, small = ui.SpacingXS;
    const float lh = ImGui::GetTextLineHeight();
    const float ty = std::floor(wp.y + (height - lh) * 0.5f);   // вертикальный центр строки
    const float icon = std::floor(lh * 0.9f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto col = [](EditorTheme::Role r) { return EditorTheme::Color32(r); };

    // --- Части и их ширина -------------------------------------------------
    const std::string project = m_project.Name();
    const std::string scene = m_scenePath.empty() ? m_scene->Name() : m_scenePath.filename().string();
    const float dot = std::floor(ui.SpacingXS * 0.75f);         // радиус точки «не сохранено»
    const float sceneW = ImGui::CalcTextSize(scene.c_str()).x + (m_sceneDirty ? small + dot * 2.0f : 0.0f);
    const int errors = m_console.ErrorsShown(), warnings = m_console.WarningsShown();
    char errText[16], warnText[16];
    std::snprintf(errText, sizeof(errText), "%d", errors);
    std::snprintf(warnText, sizeof(warnText), "%d", warnings);
    const float errW = errors > 0 ? icon + small + ImGui::CalcTextSize(errText).x : 0.0f;
    const float warnW = warnings > 0 ? icon + small + ImGui::CalcTextSize(warnText).x : 0.0f;
    const float problemsW = errW + warnW + (errW > 0.0f && warnW > 0.0f ? pad : 0.0f);
    const bool playing = m_play.Playing();
    const char* playText = InPlayMode() ? (playing ? "PLAYING" : "PAUSED") : nullptr;
    char fps[32];
    std::snprintf(fps, sizeof(fps), "%.0f", sage::Application::Get().Fps());
    const float fpsW = ImGui::CalcTextSize(T("FPS")).x + small + ImGui::CalcTextSize(fps).x;

    // По важности: сцена и проблемы — всегда, остальное — пока помещается.
    float used = sceneW;
    auto fits = [&](float w) {
        if (w <= 0.0f) return false;
        if (used + gap + w > W - pad * 2.0f) return false;
        used += gap + w;
        return true;
    };
    // Проблемы — всегда (рядом со сценой), даже если ради них не встанет остальное.
    const bool showProblems = problemsW > 0.0f;
    if (showProblems) used += gap + problemsW;
    const bool showPlay = playText && fits(ImGui::CalcTextSize(playText).x);
    const bool showProject = !project.empty() && fits(ImGui::CalcTextSize(project.c_str()).x);
    const bool showFps = fits(fpsW);
    const bool showMessage = !m_pluginStatusMessage.empty() &&
                             fits(ImGui::CalcTextSize(m_pluginStatusMessage.c_str()).x);

    // --- Слева --------------------------------------------------------------
    float x = wp.x + pad;
    if (showProject) {
        dl->AddText(ImVec2(x, ty), col(EditorTheme::Role::TextDim), project.c_str());
        x += ImGui::CalcTextSize(project.c_str()).x + gap;
    }
    {
        const ImVec2 at(x, ty);
        dl->AddText(at, col(EditorTheme::Role::Text), scene.c_str());
        x += ImGui::CalcTextSize(scene.c_str()).x;
        if (m_sceneDirty) {
            // Несохранённое — маленькой точкой, а не звёздочкой в имени файла.
            dl->AddCircleFilled(ImVec2(x + small + dot, ty + lh * 0.5f), dot, col(EditorTheme::Role::TextDim));
            x += small + dot * 2.0f;
        }
        if (ImGui::IsMouseHoveringRect(at, ImVec2(x, ty + lh)) && ImGui::IsWindowHovered())
            ImGui::SetTooltip("%s", m_sceneDirty ? T("The scene has unsaved changes (Ctrl+S saves)")
                                                 : T("The scene is saved"));
        x += gap;
    }
    // Ошибки и предупреждения — значок и число на одной оси, каждый счётчик —
    // кнопка в консоль с отбором своего уровня.
    auto counter = [&](const char* id, const char* iconName, ImVec4 c, const char* text, float w, bool errorsOnly,
                       const char* tip) {
        ImGui::SetCursorScreenPos(ImVec2(x, wp.y));
        if (ImGui::InvisibleButton(id, ImVec2(w, height))) m_console.ShowProblems(errorsOnly, !errorsOnly);
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetTooltip("%s", tip);
        EditorIcons::Overlay(dl, x, std::floor(wp.y + (height - icon) * 0.5f), icon, iconName, glm::vec3(c.x, c.y, c.z));
        dl->AddText(ImVec2(x + icon + small, ty), ImGui::GetColorU32(hovered ? EditorTheme::Color(EditorTheme::Role::Text) : c),
                    text);
        x += w;
    };
    if (showProblems) {
        // Предупреждения — приглушённо: они важны, но не кричат, как ошибки.
        if (errW > 0.0f)
            counter("##st_errors", "error", EditorTheme::Color(EditorTheme::Role::Danger), errText, errW, true,
                    T("Errors: click to show them in the Console"));
        if (errW > 0.0f && warnW > 0.0f) x += pad;
        if (warnW > 0.0f)
            counter("##st_warnings", "warn", EditorTheme::ColorAlpha(EditorTheme::Role::Warn, 0.8f), warnText, warnW,
                    false, T("Warnings: click to show them in the Console"));
        x += gap;
    }
    if (showPlay) {
        dl->AddText(ImVec2(x, ty), col(playing ? EditorTheme::Role::Ok : EditorTheme::Role::Warn), playText);
        x += ImGui::CalcTextSize(playText).x + gap;
    }
    if (showMessage) {
        dl->AddText(ImVec2(x, ty), col(EditorTheme::Role::TextDim), m_pluginStatusMessage.c_str());
        x += ImGui::CalcTextSize(m_pluginStatusMessage.c_str()).x + gap;
    }

    // --- Справа: FPS ----------------------------------------------------------
    if (showFps) {
        const float fx = wp.x + W - pad - fpsW;
        dl->AddText(ImVec2(fx, ty), col(EditorTheme::Role::TextDim), T("FPS"));
        dl->AddText(ImVec2(fx + ImGui::CalcTextSize(T("FPS")).x + small, ty), col(EditorTheme::Role::Text), fps);
    }

    // Высота строки — элементом: окно-строка знает свой размер.
    ImGui::SetCursorScreenPos(wp);
    ImGui::Dummy(ImVec2(1.0f, height));
    ImGui::EndChild();
    ImGui::PopStyleVar();
}


void EditorLayer::DrawEmptyDockHint(float minX, float minY, float maxX, float maxY) {
    const char* title = T("All panels are closed");
    const char* body = T("Windows menu returns any panel, or restore the default layout.");
    const char* action = T("Restore panels");

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 titleSize = ImGui::CalcTextSize(title);
    const ImVec2 bodySize = ImGui::CalcTextSize(body);
    const float buttonHeight = ImGui::GetFrameHeight();
    const float buttonWidth = ImGui::CalcTextSize(action).x + style.FramePadding.x * 6.0f;
    const float blockWidth = std::max(std::max(titleSize.x, bodySize.x), buttonWidth);
    const float blockHeight =
        titleSize.y + bodySize.y + buttonHeight + style.ItemSpacing.y * 4.0f;

    // Окно ровно по размеру подсказки и по центру пустого дока: полноэкранное
    // прозрачное окно перехватывало бы клики по всему редактору.
    ImGui::SetNextWindowPos(ImVec2((minX + maxX) * 0.5f, (minY + maxY) * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(blockWidth + style.WindowPadding.x * 2.0f,
                                    blockHeight + style.WindowPadding.y * 2.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (!ImGui::Begin("##SageEmptyDockHint", nullptr, flags)) {
        ImGui::End();
        return;
    }

    auto centered = [blockWidth](float width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (blockWidth - width) * 0.5f);
    };
    centered(titleSize.x);
    ImGui::TextDisabled("%s", title);
    centered(bodySize.x);
    ImGui::TextDisabled("%s", body);
    ImGui::Spacing();
    centered(buttonWidth);
    if (ImGui::Button(action, ImVec2(buttonWidth, buttonHeight))) {
        m_panelRegistry.Restore();
        m_rebuildDockLayout = true; // панель могла быть закрыта вместе со своим узлом
    }
    ImGui::End();
}

EditorHost::SceneStatsInfo EditorLayer::SceneStats() const {
    // Те же источники, что у строки состояния выше: последний кадр рендера,
    // счётчик кадров приложения и занятая процессом память.
    const sage::ecs::RenderStats& rs = m_renderer.LastStats();
    SceneStatsInfo s;
    s.Fps = sage::Application::Get().Fps();
    s.MemoryGb = (double)sage::profile::ResidentBytes() / (1024.0 * 1024.0 * 1024.0);
    s.DrawCalls = rs.Batches;
    s.Triangles = rs.Triangles;
    s.Objects = rs.Drawn;
    return s;
}

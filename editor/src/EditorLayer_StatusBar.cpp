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
#include "PanelWindows.h"

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


void EditorLayer::DrawStatusBar(float height) {
    // Строка состояния внизу хост-окна: проект | сцена(+dirty) | сущности |
    // Play-статус | сообщение плагинов | FPS.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 3));
    ImGui::BeginChild("##statusbar", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::AlignTextToFramePadding();

    // Точка состояния: зелёная в покое, жёлтая в игре. Один символ, но он
    // отвечает на вопрос «редактор жив?» без чтения строки.
    {
        const bool busy = InPlayMode();
        const ImVec4 dot = EditorTheme::Color(busy ? EditorTheme::Role::Accent
                                                   : EditorTheme::Role::Ok);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float r = Sage::UI::Get().SpacingXS * 0.75f;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f), r,
            ImGui::GetColorU32(dot));
        ImGui::Dummy(ImVec2(r * 2.0f, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0.0f, Sage::UI::Get().SpacingSM);
    }

    ImGui::TextDisabled("%s", m_project.Name().c_str());
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine();
    std::string scene = m_scenePath.empty() ? m_scene->Name() : m_scenePath.filename().string();
    ImGui::Text("%s%s", scene.c_str(), m_sceneDirty ? "*" : "");
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine(); ImGui::TextDisabled(T("Entities: %zu"), m_scene->Count());

    // Ошибки и предупреждения — В СТАТУСНОЙ СТРОКЕ, а не только в консоли.
    // Консоль легко держать свёрнутой, и тогда единственное предупреждение о
    // непрочитанном шейдере остаётся незамеченным, а сцена «просто выглядит
    // не так». Клик открывает консоль уже с нужным фильтром.
    // Считается то, что ЛЕЖИТ В КОНСОЛИ: очистил её — и статусная строка
    // перестала говорить об ошибках, которых больше нет.
    const int errorsShown = m_console.ErrorsShown(), warningsShown = m_console.WarningsShown();
    if (errorsShown > 0 || warningsShown > 0) {
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine();
        const bool hasErrors = errorsShown > 0;
        const ImVec4 col = hasErrors ? ImVec4(0.95f, 0.40f, 0.40f, 1.0f)
                                     : ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
        EditorIcons::Inline(hasErrors ? "error" : "warn", glm::vec3(col.x, col.y, col.z));
        ImGui::SameLine(0.0f, 0.0f);
        // Склонение по-русски: 1 ошибка, 2 ошибки, 5 ошибок. Мелочь, но
        // «1 ошибок» в статусной строке читается как недоделка интерфейса.
        auto plural = [](int n, const char* one, const char* few, const char* many) {
            const int n100 = n % 100, n10 = n % 10;
            if (n100 >= 11 && n100 <= 14) return many;
            if (n10 == 1) return one;
            if (n10 >= 2 && n10 <= 4) return few;
            return many;
        };
        ImGui::TextColored(col, "%d %s, %d %s", errorsShown,
                           plural(errorsShown, T("error"), T("errors"), T("errors (many)")),
                           warningsShown,
                           plural(warningsShown, T("warning"), T("warnings"),
                                  T("warnings (many)")));
        if (ImGui::IsItemClicked()) m_console.ShowProblems();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Show the problems: what broke and how to fix it"));
    }

    if (InPlayMode()) {
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine();
        bool playing = m_play.Playing();
        ImGui::TextColored(playing ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                           playing ? "PLAYING" : "PAUSED");
    }
    if (!m_pluginStatusMessage.empty()) {
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine(); ImGui::TextDisabled("%s", m_pluginStatusMessage.c_str());
    }

    // --- СПРАВА: показатели сцены и версия движка ---------------------------
    //
    // Пары «подпись — значение», а не слитная строка «meshes 8/8 culled 0
    // batches 4»: слитную читают только те, кто её писал. Подпись приглушена,
    // значение обычным цветом — глаз находит число, не читая всю строку.
    //
    // Собирается СПРАВА НАЛЕВО: ширина считается заранее, и при узком окне
    // лишние пары просто не рисуются, а не наезжают на левую часть.
    {
        const sage::ecs::RenderStats& rs = m_renderer.LastStats();
        char fps[32], mem[32], draws[32], tris[32], objs[32];
        std::snprintf(fps, sizeof(fps), "%.0f", sage::Application::Get().Fps());
        std::snprintf(mem, sizeof(mem), "%.2f %s",
                      (double)sage::profile::ResidentBytes() / (1024.0 * 1024.0 * 1024.0), T("GB"));
        std::snprintf(draws, sizeof(draws), "%d", rs.Batches);
        // Разряды разделяются пробелом: «89 442» читается с одного взгляда,
        // «89442» — нет. Пробелом, а не запятой: запятая в русском тексте
        // означает дробную часть.
        {
            char raw[32];
            std::snprintf(raw, sizeof(raw), "%lld", rs.Triangles);
            const int len = (int)std::strlen(raw);
            int out = 0;
            for (int i = 0; i < len && out < (int)sizeof(tris) - 2; ++i) {
                if (i > 0 && (len - i) % 3 == 0) tris[out++] = ' ';
                tris[out++] = raw[i];
            }
            tris[out] = '\0';
        }
        std::snprintf(objs, sizeof(objs), "%d", rs.Drawn);

        struct Stat { const char* Label; const char* Value; };
        const Stat stats[] = {
            {T("FPS"), fps},
            {T("Mem"), mem},
            {T("Draw Calls"), draws},
            {T("Triangles"), tris},
            {T("Objects"), objs},
        };
        const std::string version = std::string("SAGE Engine ") + kSageEngineVersion;

        const float gap = Sage::UI::Get().SpacingLG;
        float need = ImGui::CalcTextSize(version.c_str()).x + Sage::UI::Get().IconSize + gap;
        int shown = 0;
        for (int i = (int)IM_ARRAYSIZE(stats) - 1; i >= 0; --i) {
            const float w = ImGui::CalcTextSize(stats[i].Label).x +
                            ImGui::CalcTextSize(stats[i].Value).x +
                            Sage::UI::Get().SpacingSM + gap;
            if (need + w > ImGui::GetWindowWidth() * 0.55f) break;
            need += w;
            ++shown;
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - need);
        for (int i = (int)IM_ARRAYSIZE(stats) - shown; i < (int)IM_ARRAYSIZE(stats); ++i) {
            ImGui::TextDisabled("%s", stats[i].Label);
            ImGui::SameLine(0.0f, Sage::UI::Get().SpacingSM);
            ImGui::TextUnformatted(stats[i].Value);
            ImGui::SameLine(0.0f, gap);
        }
        EditorIcons::Inline("cube", glm::vec3(EditorTheme::Color(EditorTheme::Role::Accent).x,
                                              EditorTheme::Color(EditorTheme::Role::Accent).y,
                                              EditorTheme::Color(EditorTheme::Role::Accent).z));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled("%s", version.c_str());
    }

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

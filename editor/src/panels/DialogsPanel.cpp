#include "DialogsPanel.h"

#include "../ProjectTemplates.h"
#include "../ProjectTemplateCover.h"
#include "EditorHost.h"
#include "Project.h"

#include <imgui.h>

#include "EditorIcons.h"
#include "EditorTheme.h"
#include "../GameBuilder.h"
#include "../ProjectLauncher/ProjectDatabase.h"
#include "sage/core/Paths.h"
#include <filesystem>

#include <cstdio>
#include "../Localization.h"

namespace fs = std::filesystem;

namespace {
// Размер окна в пикселях интерфейса: при крупном масштабе (4K, 150 %) окно
// растёт вместе с текстом, а не обрезает его.
float Scaled(float px) { return px * EditorTheme::UiScale(); }
} // namespace

DialogsPanel::DialogsPanel() {
    // Дефолтная папка диалогов — рядом с бинарником; сборка игр — в dist/.
    std::snprintf(m_projectDir, sizeof(m_projectDir), "%s", fs::current_path().string().c_str());
    // Папка сборки ставится при открытии окна — от проекта (см. Build Game).
}

void DialogsPanel::Open(const char* name) {
    m_error.clear();
    ImGui::OpenPopup(name);
}

void DialogsPanel::Browse(const FileBrowser::Config& cfg, char* buffer, size_t size) {
    m_browseTarget = buffer;
    m_browseTargetSize = size;
    FileBrowser::Config c = cfg;
    // Начинаем с того, что уже введено: если человек правит путь, обзор обязан
    // открыться ТАМ ЖЕ, а не в домашней папке — иначе он каждый раз идёт весь
    // путь заново.
    if (buffer[0]) {
        std::error_code ec;
        fs::path typed(buffer);
        if (fs::is_directory(typed, ec)) c.StartDir = typed;
        else if (typed.has_parent_path() && fs::is_directory(typed.parent_path(), ec))
            c.StartDir = typed.parent_path();
    }
    m_browser.Open(c);
}

void DialogsPanel::BrowseButton(const char* id, const FileBrowser::Config& cfg, char* buffer,
                                size_t size) {
    ImGui::SameLine();
    ImGui::PushID(id);
    if (EditorIcons::Button("folder", T("Browse..."))) Browse(cfg, buffer, size);
    ImGui::PopID();
}

void DialogsPanel::Draw(EditorHost& host) {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();

    // Результат обзора кладём в буфер того поля, ради которого его открыли.
    if (m_browser.Draw() && m_browseTarget) {
        std::snprintf(m_browseTarget, m_browseTargetSize, "%s",
                      m_browser.Result().string().c_str());
        m_browseTarget = nullptr;
        m_error.clear();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("New Project" "###New Project"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText(T("Name"), m_projectName, sizeof(m_projectName));
        ImGui::InputText(T("Location"), m_projectDir, sizeof(m_projectDir));
        {
            FileBrowser::Config c;
            c.Title = T("Where to create the project");
            c.Mode = FileBrowser::PickMode::PickFolder;
            BrowseButton("newproj", c, m_projectDir, sizeof(m_projectDir));
        }
        ImGui::TextDisabled("%s", T("Creates <Location>/<Name>/project.sageproj + scenes/ + assets/"));

        // Шаблон — С ЧЕГО начинается проект. Пока выбора не было, каждый новый
        // проект начинался с девяти демо-объектов, которые первым делом
        // удаляли.
        ImGui::SeparatorText(T("Start from"));
        // Карточками с обложкой — тем же виджетом, что и в стартовом окне.
        // Двух похожих списков шаблонов в редакторе быть не должно: они
        // разъедутся ровно так же, как разъезжались подписи и порядок.
        for (size_t i = 0; i < ProjectTemplates().size(); ++i) {
            const ProjectTemplate& tpl = ProjectTemplates()[i];
            if (i > 0) ImGui::SameLine();
            if (ProjectTemplateCard(tpl, m_templateId == tpl.Id, 190.0f)) m_templateId = tpl.Id;
        }

        // Пояснение выбранного шаблона — под рядом карточек, а не только
        // подсказкой при наведении: у мыши, ведущей к кнопке «Создать», подсказки
        // уже нет, а решение принимают именно в этот момент.
        const ProjectTemplate* chosen = FindProjectTemplate(m_templateId);
        if (chosen) {
            ImGui::TextDisabled("%s", T(chosen->Summary));
            if (chosen->Note[0] != '\0') ImGui::TextWrapped("%s", T(chosen->Note));
        }
        // Недоступный шаблон гасит кнопку ЗДЕСЬ. Отказ после нажатия человек
        // читает как поломку: он выбрал то, что ему предложили.
        const bool blocked = chosen && !ProjectTemplateAvailable(*chosen);
        if (blocked) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "%s",
                               T("This template is not installed next to the editor"));
        }

        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button(T("Create"), ImVec2(120, 0))) {
            std::string err;
            if (host.CreateProject(m_projectDir, m_projectName, m_templateId, err)) {
                ImGui::CloseCurrentPopup();
            } else {
                m_error = err;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Open Project" "###Open Project"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText(T("Path"), m_openPath, sizeof(m_openPath));
        {
            FileBrowser::Config c;
            c.Title = T("Open project");
            c.Mode = FileBrowser::PickMode::OpenFile;
            c.Filters = {".sageproj"};
            c.FilterLabel = T("Projects (*.sageproj)");
            BrowseButton("openproj", c, m_openPath, sizeof(m_openPath));
        }
        ImGui::SameLine();
        {
            FileBrowser::Config c;
            c.Title = T("Open the project folder");
            c.Mode = FileBrowser::PickMode::PickFolder;
            BrowseButton("openprojdir", c, m_openPath, sizeof(m_openPath));
        }
        ImGui::TextDisabled("%s", T("Path to project.sageproj or to a project folder"));
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button(T("Open"), ImVec2(120, 0))) {
            std::string err;
            if (host.OpenProject(m_openPath, err)) {
                ImGui::CloseCurrentPopup();
            } else {
                m_error = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Save Scene As" "###Save Scene As"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText(T("File name"), m_sceneName, sizeof(m_sceneName));
        fs::path target =
            host.CurrentProject().ScenesDir() / (std::string(m_sceneName) + ".sage");
        ImGui::TextDisabled("-> %s", target.string().c_str());
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button(T("Save"), ImVec2(120, 0))) {
            host.CurrentScene().SetName(m_sceneName);
            if (host.SaveSceneToFile(target)) ImGui::CloseCurrentPopup();
            else m_error = "Save failed (see Console)";
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // --- СБОРКА ИГРЫ -----------------------------------------------------------
    //
    // ПАПКА ПО УМОЛЧАНИЮ — В ПРОЕКТЕ (<проект>/Builds), а не рядом с редактором:
    // сборка — это результат ПРОЕКТА, и складывать игры всех проектов в папку
    // установки движка (там её и находили — в Downloads/SageEditor-Windows/dist)
    // значило смешивать чужое с программой, которую обновляют заменой папки.
    //
    // Сборка идёт В ФОНЕ (GameBuilder): окно показывает полосу хода, сборку
    // можно отменить, редактор не висит.
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Scaled(640.0f), 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(T("Build Game" "###Build Game"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        Project& project = host.CurrentProject();
        GameBuilder& builder = host.Builder();
        const bool running = builder.Running();
        const std::string projectKey = sage::PathToUtf8(project.Dir());
        if (m_buildDirProject != projectKey) {
            m_buildDirProject = projectKey;
            std::error_code absEc;
            std::snprintf(m_buildDir, sizeof(m_buildDir), "%s",
                          sage::PathToUtf8(fs::absolute(project.Dir() / "Builds", absEc).lexically_normal()).c_str());
        }
        const float width = Scaled(600.0f);

        ImGui::TextUnformatted(project.Name().c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextDisabled("%s", T("Player and project are packed into a folder ready to run. "
                                    "All game files go into one encrypted game.sagepak."));
        ImGui::PopTextWrapPos();

        ImGui::SeparatorText(T("Output folder"));
        ImGui::BeginDisabled(running);
        {
            const float browseW = EditorIcons::LabeledWidth(ImGui::GetFontSize(), T("Browse...")) +
                                  ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(width - browseW - ImGui::GetStyle().ItemSpacing.x);
            ImGui::InputText("##builddir", m_buildDir, sizeof(m_buildDir));
            FileBrowser::Config c;
            c.Title = T("Where to build the game");
            c.Mode = FileBrowser::PickMode::PickFolder;
            BrowseButton("builddir", c, m_buildDir, sizeof(m_buildDir));
        }
        ImGui::EndDisabled();
        {
            const fs::path exe = sage::PathFromUtf8(m_buildDir) / project.Name() /
#ifdef _WIN32
                                 (project.Name() + ".exe");
#else
                                 project.Name();
#endif
            ImGui::TextDisabled("%s", T("The game will be:"));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextWrapped("%s", sage::PathToUtf8(exe.lexically_normal()).c_str());
            ImGui::PopTextWrapPos();
        }

        ImGui::SeparatorText(T("What goes in"));
        ImGui::BulletText("%s %s", T("Start scene:"),
                          project.StartScene().empty() ? T("main.sage, else the first by name")
                                                       : (project.StartScene() + ".sage").c_str());
        ImGui::BulletText("%s", T("Scenes, scripts, models, textures, sounds, materials, interfaces"));
        ImGui::BulletText("%s", T("Engine shaders and fonts"));

        // ПРЕДУПРЕЖДЕНИЕ ДО НАЖАТИЯ, А НЕ ОШИБКА ПОСЛЕ. Игры без сцены не
        // бывает: плеер открыл бы пустой экран, и человек, получивший такую
        // сборку, увидел бы чёрное окно без единого объяснения.
        const bool hasScene = host.HasAnyScene();
        if (!hasScene) {
            ImGui::Spacing();
            EditorIcons::Inline("warn", glm::vec3(EditorTheme::Color(EditorTheme::Role::Warn).x,
                                                  EditorTheme::Color(EditorTheme::Role::Warn).y,
                                                  EditorTheme::Color(EditorTheme::Role::Warn).z));
            ImGui::SameLine(0.0f, EditorIcons::TextGap());
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                               T("The project has no scene — there is nothing to run"));
            ImGui::TextDisabled("%s", T("Create a scene and save it into scenes/ of the project."));
        }

        // Ход сборки — всегда на своём месте, чтобы окно не прыгало.
        ImGui::Spacing();
        if (running || builder.Finished()) {
            const std::string stage = running ? builder.Stage()
                                   : builder.Succeeded() ? std::string(T("Done"))
                                                         : std::string(T("Build failed"));
            ImGui::ProgressBar(running ? builder.Progress() : (builder.Succeeded() ? 1.0f : 0.0f),
                               ImVec2(width, 0.0f), stage.c_str());
        }
        if (builder.Finished() && !running) {
            if (builder.Succeeded()) {
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Ok), T("Built: %s"),
                                   sage::PathToUtf8(builder.GameDir()).c_str());
            } else if (!builder.Error().empty()) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s",
                                   builder.Error().c_str());
                ImGui::PopTextWrapPos();
            }
        }
        if (!m_error.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s", m_error.c_str());
            ImGui::PopTextWrapPos();
        }

        ImGui::Separator();
        const ImVec2 btn(Scaled(150.0f), 0.0f);
        if (running) {
            if (ImGui::Button(T("Cancel build"), btn)) builder.Cancel();
        } else {
            ImGui::BeginDisabled(!hasScene);
            if (ImGui::Button(T("Build"), btn)) {
                std::string err;
                m_error.clear();
                if (!host.StartBuildGame(sage::PathFromUtf8(m_buildDir), err)) m_error = err;
            }
            ImGui::EndDisabled();
            if (builder.Finished() && builder.Succeeded()) {
                ImGui::SameLine();
                if (ImGui::Button(T("Open folder"), btn))
                    Sage::Launcher::RevealInFileManager(builder.GameDir());
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(running);
        if (ImGui::Button(T("Close"), btn)) ImGui::CloseCurrentPopup();
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Open Scene" "###Open Scene"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText(T("Path"), m_openPath, sizeof(m_openPath));
        {
            FileBrowser::Config c;
            c.Title = T("Open scene");
            c.Filters = {".sage"};
            c.FilterLabel = T("Scenes (*.sage)");
            c.StartDir = host.CurrentProject().ScenesDir();
            BrowseButton("openscene", c, m_openPath, sizeof(m_openPath));
        }
        ImGui::TextDisabled("%s", T("Path to a .sage file (or double-click in the Assets panel)"));
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button(T("Open"), ImVec2(120, 0))) {
            if (host.LoadSceneFromFile(m_openPath)) ImGui::CloseCurrentPopup();
            else m_error = "Load failed (see Console)";
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

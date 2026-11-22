#include "DialogsPanel.h"

#include "EditorHost.h"
#include "Project.h"

#include <imgui.h>

#include "EditorIcons.h"
#include "EditorTheme.h"
#include "../GameBuilder.h"
#include "../ProjectLauncher/ProjectDatabase.h"
#include "sage/core/Paths.h"
#include "sage/assets/GameInfo.h"
#include <filesystem>

#include <cstdio>
#include "../Localization.h"

namespace fs = std::filesystem;

namespace {
// Размер окна в пикселях интерфейса: при крупном масштабе (4K, 150 %) окно
// растёт вместе с текстом, а не обрезает его.
float Scaled(float px) { return px * EditorTheme::EffectiveScale(); }

fs::path AbsNormal(const fs::path& p) {
    std::error_code ec;
    fs::path r = fs::absolute(p, ec).lexically_normal();
    if (!r.empty() && !r.has_filename()) r = r.parent_path(); // убрать хвостовой «/»
    return r;
}

// Каталог вывода не может быть корнем проекта или его родителем: иначе сборка
// исключила бы содержимое проекта из пакета. Сравнение — по компонентам пути,
// а не по строковому префиксу ("/a/My" не предок "/a/MyGame").
bool OutputDirOverlapsProject(const fs::path& out, const fs::path& project) {
    const fs::path rel = AbsNormal(project).lexically_relative(AbsNormal(out));
    if (rel.empty()) return false;
    return *rel.begin() != "..";
}

// Иконки платформ — ближайшие уже встроенные Tabler-глифы (источника Tabler
// Webfont для brand-windows/ubuntu/apple в сборке нет).
const char* TargetIcon(const std::string& id) {
    if (id == "windows-x64") return "window";
    if (id == "linux-x64") return "console";
    return "game";
}
} // namespace

DialogsPanel::DialogsPanel() {
    // Дефолтная папка диалогов — рядом с бинарником; сборка игр — в dist/.
    std::snprintf(m_projectDir, sizeof(m_projectDir), "%s", sage::PathToUtf8(fs::current_path()).c_str());
    // Папка сборки ставится при открытии окна — от проекта (см. Build Game).
}

void DialogsPanel::Open(const char* name) {
    m_error.clear();
    if (std::string(name) == "Build Game") m_buildRefresh = true;
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
        const fs::path typed = sage::PathFromUtf8(buffer);
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
                      sage::PathToUtf8(m_browser.Result()).c_str());
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
        EditorTheme::Hint(T("Creates <Location>/<Name>/project.sageproj + scenes/ + assets/"));

        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button(T("Create"), ImVec2(120, 0))) {
            std::string err;
            if (host.CreateProject(m_projectDir, m_projectName, err)) {
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
        EditorTheme::Hint(T("Path to project.sageproj or to a project folder"));
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
        ImGui::TextDisabled("-> %s", sage::PathToUtf8(target).c_str());
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
        if (m_buildDirProject != projectKey) m_buildRefresh = true;
        if (m_buildRefresh) {
            m_buildRefresh = false;
            m_targets = host.BuildTargets();
            // Имя игры — по правилам GameBuild.cpp, а не project.Name().
            sage::assets::GameInfo info;
            const bool hasInfo = sage::assets::LoadGameInfo(project.Dir(), info);
            if (!hasInfo || info.Name.empty()) info.Name = project.Name();
            m_buildFileName = project.Name().empty() || hasInfo ? info.FileName() : project.Name();
            // Нативная платформа, иначе первая доступная; выбор пользователя сохраняем.
            bool keep = false;
            for (const BuildTargetInfo& t : m_targets) keep |= (t.Id == m_targetId && t.Available);
            if (!keep) {
                m_targetId.clear();
#if defined(_WIN32)
                const char* native = "windows-x64";
#elif defined(__APPLE__)
                const char* native = "macos-arm64";
#else
                const char* native = "linux-x64";
#endif
                for (const BuildTargetInfo& t : m_targets)
                    if (t.Available && t.Id == native) m_targetId = t.Id;
                if (m_targetId.empty())
                    for (const BuildTargetInfo& t : m_targets)
                        if (t.Available) { m_targetId = t.Id; break; }
            }
        }
        const float width = Scaled(600.0f);

        ImGui::TextUnformatted(m_buildFileName.c_str());

        // --- ПЛАТФОРМА: три карточки; недоступная не выбирается и говорит почему.
        ImGui::SeparatorText(T("Target Platform"));
        {
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float cardW = (width - gap * 2.0f) / 3.0f;
            const float cardH = Scaled(120.0f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImGui::BeginDisabled(running);
            for (size_t i = 0; i < m_targets.size(); ++i) {
                const BuildTargetInfo& t = m_targets[i];
                if (i) ImGui::SameLine(0.0f, gap);
                ImGui::PushID((int)i);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const bool clicked = ImGui::InvisibleButton("##card", ImVec2(cardW, cardH));
                const bool hovered = ImGui::IsItemHovered();
                if (clicked && t.Available) m_targetId = t.Id;
                if (hovered && !t.Available && !t.UnavailableReason.empty())
                    ImGui::SetTooltip("%s", t.UnavailableReason.c_str());
                const bool selected = t.Available && t.Id == m_targetId;
                const float rounding = Scaled(6.0f);
                const ImVec2 max(pos.x + cardW, pos.y + cardH);
                dl->AddRectFilled(pos, max,
                    selected ? EditorTheme::Color32Alpha(EditorTheme::Role::Accent, 0.18f)
                             : EditorTheme::Color32(hovered && t.Available ? EditorTheme::Role::Hover
                                                                           : EditorTheme::Role::SurfaceAlt),
                    rounding);
                dl->AddRect(pos, max,
                    selected ? EditorTheme::Color32(EditorTheme::Role::Accent)
                             : EditorTheme::Color32(EditorTheme::Role::Line),
                    rounding, 0, selected ? 2.0f : 1.0f);
                // Галочка выбранной карточки — тремя штрихами ImDrawList, без
                // нового значка в шрифте.
                if (selected) {
                    const float r = Scaled(9.0f);
                    const ImVec2 c(max.x - Scaled(8.0f) - r, pos.y + Scaled(8.0f) + r);
                    dl->AddCircleFilled(c, r, EditorTheme::Color32(EditorTheme::Role::Accent));
                    const ImVec2 pts[3] = {ImVec2(c.x - r * 0.45f, c.y + r * 0.02f),
                                           ImVec2(c.x - r * 0.1f, c.y + r * 0.38f),
                                           ImVec2(c.x + r * 0.5f, c.y - r * 0.32f)};
                    dl->AddPolyline(pts, 3, EditorTheme::Color32(EditorTheme::Role::TextOnAccent),
                                    ImDrawFlags_None, Scaled(2.0f));
                }
                const ImU32 textCol = EditorTheme::Color32(t.Available ? EditorTheme::Role::Text
                                                                       : EditorTheme::Role::TextFaint);
                const float iconSize = Scaled(32.0f);
                const ImVec4 ic = EditorTheme::Color(t.Available ? EditorTheme::Role::Text
                                                                 : EditorTheme::Role::TextFaint);
                EditorIcons::Overlay(pos.x + (cardW - iconSize) * 0.5f, pos.y + Scaled(8.0f), iconSize,
                                     TargetIcon(t.Id), glm::vec3(ic.x, ic.y, ic.z));
                float y = pos.y + Scaled(8.0f) + iconSize + Scaled(4.0f);
                auto centered = [&](const char* txt, ImU32 col) {
                    const float w = ImGui::CalcTextSize(txt).x;
                    dl->AddText(ImVec2(pos.x + (cardW - w) * 0.5f, y), col, txt);
                    y += ImGui::GetTextLineHeightWithSpacing();
                };
                centered(T(t.Name.c_str()), textCol);
                centered(t.Architecture.c_str(), EditorTheme::Color32(EditorTheme::Role::TextDim));
                centered(t.Available ? T("Ready") : T("Not installed"),
                         EditorTheme::Color32(t.Available ? EditorTheme::Role::Ok
                                                          : EditorTheme::Role::TextFaint));
                ImGui::PopID();
            }
            ImGui::EndDisabled();
        }
        // Причина недоступности — прямо в окне, не в консоли.
        const BuildTargetInfo* selectedTarget = nullptr;
        for (const BuildTargetInfo& t : m_targets)
            if (t.Available && t.Id == m_targetId) selectedTarget = &t;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        if (!selectedTarget) {
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                               T("Runtime unavailable: no SagePlayer is installed for any platform"));
        }
        for (const BuildTargetInfo& t : m_targets)
            if (!t.Available)
                ImGui::TextDisabled("%s: %s", T(t.Name.c_str()), t.UnavailableReason.c_str());
        ImGui::PopTextWrapPos();

        ImGui::SeparatorText(T("Output directory"));
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
        const fs::path outDir = AbsNormal(sage::PathFromUtf8(m_buildDir));
        const bool outOk = !outDir.empty() && !OutputDirOverlapsProject(outDir, project.Dir());
        {
            const bool isExe = selectedTarget && selectedTarget->Id == "windows-x64";
            const fs::path exe = outDir / sage::PathFromUtf8(m_buildFileName) /
                                 sage::PathFromUtf8(m_buildFileName + (isExe ? ".exe" : ""));
            ImGui::SeparatorText(T("Final output"));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextWrapped("%s", sage::PathToUtf8(exe).c_str());
            if (!outOk)
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s",
                                   T("The output directory must not be the project folder or its parent"));
            else
                ImGui::TextDisabled("%s", T("Existing build will be replaced after successful build"));
            ImGui::PopTextWrapPos();
        }

        ImGui::SeparatorText(T("What goes in"));
        // Состав сборки одинаков у любого проекта — в подсказку, а не списком в окне.
        EditorTheme::Hint(T("Scenes, scripts, models, textures, sounds, materials, interfaces; engine shaders and fonts"));
        ImGui::BulletText("%s %s", T("Start scene:"),
                          project.StartScene().empty() ? T("main.sage, else the first by name")
                                                       : (project.StartScene() + ".sage").c_str());

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
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Ok), T("Built: %s"),
                                   sage::PathToUtf8(builder.GameDir()).c_str());
                ImGui::PopTextWrapPos();
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
            ImGui::BeginDisabled(!hasScene || !selectedTarget || !outOk);
            if (ImGui::Button(T("Build"), btn)) {
                std::string err;
                m_error.clear();
                if (!host.StartBuildGame(outDir, err, m_targetId)) {
                    m_error = err;
                    m_buildRefresh = true; // runtime мог исчезнуть: карточки покажут правду
                }
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
        EditorTheme::Hint(T("Path to a .sage file (or double-click in the Assets panel)"));
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

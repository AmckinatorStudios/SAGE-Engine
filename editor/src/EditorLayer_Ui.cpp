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
#include "EditorLayer.h"
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
#include "sage/core/Application.h"
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
#include "sage/render/ParticlePresets.h"
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

namespace {

// Пересечение луча с произвольным AABB [bmin, bmax] в локальном пространстве
// объекта (slab-тест). Возвращает t входа (>=0) или отрицательное при промахе.
float RayBox(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& bmin, const glm::vec3& bmax) {
    glm::vec3 inv = 1.0f / rd; // IEEE inf при нулевой компоненте — slab-тест это переживает
    glm::vec3 t0 = (bmin - ro) * inv;
    glm::vec3 t1 = (bmax - ro) * inv;
    glm::vec3 tmin = glm::min(t0, t1), tmax = glm::max(t0, t1);
    float tNear = std::max({tmin.x, tmin.y, tmin.z});
    float tFar  = std::min({tmax.x, tmax.y, tmax.z});
    if (tNear > tFar || tFar < 0.0f) return -1.0f;
    return tNear >= 0.0f ? tNear : tFar;
}

// Луч vs единичный куб [-0.5,0.5]^3 — маркеры невидимых сущностей (камера/свет).
float RayUnitCube(const glm::vec3& ro, const glm::vec3& rd) {
    return RayBox(ro, rd, glm::vec3(-0.5f), glm::vec3(0.5f));
}

constexpr float kStatusBarHeight = 26.0f;

} // namespace


// ПЕРЕКЛЮЧЕНИЕ ПРОСТРАНСТВА — не «режим», а смена состава панелей.
//
// Выделение при этом СБРАСЫВАЕТСЯ, и это не мелочь: в сцене выбирают объекты, в
// вёрстке — элементы, и выбранный куб, оставшийся выбранным после перехода к
// интерфейсу, означает инспектор, показывающий меш там, где ждут раскладку.
void EditorLayer::SetWorkspace(EditorWorkspace workspace) {
    if (workspace == m_workspace) return;
    // ВЫБОР ИНТЕРФЕЙСА — ДО СБРОСА ВЫДЕЛЕНИЯ. Самый частый путь в вёрстку:
    // выбрали в сцене объект интерфейса (или его элемент) и переключились. По
    // выделению и понятно, какой интерфейс человек собрался править; после
    // сброса эта подсказка потеряна.
    if (workspace == EditorWorkspace::Interface) ResolveCurrentInterface();
    m_workspace = workspace;
    m_selection.Clear();
    // Своя раскладка у каждого пространства строится при первом показе: узла с
    // таким именем ImGui ещё не знает, и BuildXxxDockLayout сработает сам.
    if (workspace == EditorWorkspace::Interface) {
        m_uiViewport.RequestFit();
        m_uiViewport.RequestFocus();
    } else {
        m_viewport.RequestFocus();
    }
}


// КАКОЙ ИНТЕРФЕЙС ВЕРСТАЕМ.
//
// Правило одно на редактор, и вот почему оно здесь, а не в панели: дерево,
// холст и инспектор обязаны показывать ОДИН И ТОТ ЖЕ интерфейс. Три ответа на
// этот вопрос в трёх панелях — это дерево от одного экрана поверх холста
// другого, и объяснить такую картинку нечем.
void EditorLayer::ResolveCurrentInterface() {
    auto valid = [&](int id) {
        if (id < 0) return false;
        if (id == 0) return true;  // «Без интерфейса» — строка, а не объект
        GameObject obj = m_scene->Get(id);
        return obj.Valid() && m_scene->Registry().all_of<sage::ui::InterfaceComponent>(obj.Entity());
    };

    // 1. Тот, что и был, если он ещё жив: переключение пространств туда-обратно
    // не должно терять место работы.
    if (valid(m_currentInterface)) return;

    // 2. Интерфейс ВЫДЕЛЕННОГО объекта: пришли из сцены, выбрав меню или его
    // кнопку, — значит правят именно его.
    if (m_selection.Primary() >= 0) {
        GameObject sel = m_scene->Get(m_selection.Primary());
        if (sel.Valid()) {
            const entt::entity owner = sage::ui::InterfaceOf(*m_scene, sel.Entity());
            if (owner != entt::null) {
                const IdComponent* id = m_scene->Registry().try_get<IdComponent>(owner);
                m_currentInterface = id ? id->Id : -1;
                if (valid(m_currentInterface)) return;
            }
        }
    }

    // 3. Первый в сцене — по тому же порядку, в каком они показываются.
    const std::vector<entt::entity> all = sage::ui::SortedInterfaces(*m_scene);
    if (!all.empty()) {
        const IdComponent* id = m_scene->Registry().try_get<IdComponent>(all.front());
        m_currentInterface = id ? id->Id : -1;
        return;
    }

    // 4. Интерфейсов нет вовсе. Если в сцене есть элементы без интерфейса (их
    // собрал код или скрипт), показываем их — иначе панель выглядела бы
    // сломанной при непустой сцене.
    m_currentInterface = sage::ui::InterfaceRoots(*m_scene, entt::null).empty() ? -1 : 0;
}

sage::ui::UIScope EditorLayer::UiScope() const {
    // В пространстве сцены — всё как в игре: там интерфейс не правят, а видят.
    if (m_workspace != EditorWorkspace::Interface) return sage::ui::UIScope::All();
    if (m_currentInterface < 0) return sage::ui::UIScope::All();
    if (m_currentInterface == 0) return sage::ui::UIScope::Only(entt::null);
    GameObject obj = m_scene->Get(m_currentInterface);
    if (!obj.Valid()) return sage::ui::UIScope::All();
    return sage::ui::UIScope::Only(obj.Entity());
}

void EditorLayer::BuildInterfaceDockLayout(unsigned int dockspaceId) {
    // У вёрстки другой главный: ХОЛСТ, а не вьюпорт сцены. Всё остальное
    // стоит вокруг него так же, как в пространстве сцены, — привычка к
    // расположению панелей важнее, чем оригинальность раскладки.
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.26f, nullptr, &center);

    auto dock = [](const char* id, ImGuiID node) {
        if (!panelwindows::Detached(id)) ImGui::DockBuilderDockWindow(id, node);
    };
    dock("InterfaceHierarchy", left);
    dock("InterfaceInspector", right);
    // СВОИ ОКНА ЭТОГО ПРОСТРАНСТВА, а не общие с пространством сцены: пока
    // окно было одно на оба, эта строка ЗАБИРАЛА его у раскладки сцены, и
    // вернуть его туда было уже некуда (см. PanelWindowId.h).
    dock("AssetsUI", bottom);
    dock("ConsoleUI", bottom);
    dock("AnimationUI", bottom);
    // Холст докается ПЕРВЫМ — он и есть вкладка по умолчанию. Предпросмотр
    // рядом с ним: переключаться между «верстаю» и «смотрю» надо одним щелчком,
    // а не раскладкой заново.
    dock("InterfaceViewport", center);
    dock("InterfacePreview", center);
    ImGui::DockBuilderFinish(dockspaceId);

    // ПО ИДЕНТИФИКАТОРУ, а не по имени: имя панели переводится («Canvas» /
    // «Холст»), и SetWindowFocus по строке в русском редакторе не находил
    // ничего — молча.
    if (ImGuiWindow* canvas = ImGui::FindWindowByID(ImHashStr("InterfaceViewport")))
        ImGui::FocusWindow(canvas);
}

void EditorLayer::BuildDefaultDockLayout(unsigned int dockspaceId) {
    // Пересобираем узлы доккинга с нуля: Viewport+Game в центре (табами),
    // панели вокруг.
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.21f, nullptr, &center);
    ImGuiID left  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.23f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);

    // Сброс раскладки возвращает и состав ОКОН, а не только расстановку
    // вкладок: окно, уехавшее на монитор, которого больше нет, иначе не
    // вернуть ничем (см. PanelWindows.h).
    panelwindows::ResetToDefaults();
    // Панель, живущая своим окном системы, в док не заводится: DockBuilder
    // втянул бы её обратно вкладкой, отменив то, о чём человека не спрашивали.
    auto dock = [](const char* id, ImGuiID node) {
        if (!panelwindows::Detached(id)) ImGui::DockBuilderDockWindow(id, node);
    };

    dock("Hierarchy", left);
    dock("Lighting", right);
    dock("Inspector", right);
    dock("Console", bottom);
    dock("Assets", bottom);
    dock("Animation", bottom);
    // Viewport докается ПЕРВЫМ в центральный узел — так он и есть таб по
    // умолчанию (первый добавленный к узлу становится выбранным). Раньше первым
    // шёл Game, из-за чего редактор открывался на «игровом окне» без пикинга/
    // гизмо/аутлайна — выглядело как «выделение не работает». Game выходит
    // вперёд при входе в Play (GamePanel::RequestFocus).
    dock("Viewport", center);
    dock("Game", center);
    // Редактора интерфейса здесь больше нет вовсе: вёрстка — ОТДЕЛЬНОЕ рабочее
    // пространство со своими панелями и своей раскладкой (см. EditorTypes.h).
    ImGui::DockBuilderFinish(dockspaceId);

    ImGui::SetWindowFocus("Viewport");
}

// Help > About SAGE — версия движка + таблица версий ВСЕХ подсистем (пока все
// v1). Единый источник — sage::EngineSystems() (тот же список, что в лог старта).
void EditorLayer::DrawAboutWindow() {
    if (!m_showAbout) return;
    ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("About SAGE" "###About SAGE"), &m_showAbout)) { ImGui::End(); return; }

    ImGui::Text(T("SAGE Engine %s"), kSageEngineVersion);
    ImGui::TextDisabled("%s", T("A modular 3D engine: ECS, RHI, PBR, physics, scripting, UI."));
    ImGui::Spacing();
    const auto& systems = sage::EngineSystems();
    ImGui::Text(T("Subsystems: %zu (all v1)"), systems.size());
    ImGui::Separator();

    if (ImGui::BeginTable("##systems", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Ver", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const sage::SystemVersion& s : systems) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.Name);
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%s", s.Tag().c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", s.Summary);
        }
        ImGui::EndTable();
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
    if (m_console.ErrorCount() > 0 || m_console.WarnCount() > 0) {
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine();
        const bool hasErrors = m_console.ErrorCount() > 0;
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
        ImGui::TextColored(col, "%d %s, %d %s", m_console.ErrorCount(),
                           plural(m_console.ErrorCount(), T("error"), T("errors"), T("errors (many)")),
                           m_console.WarnCount(),
                           plural(m_console.WarnCount(), T("warning"), T("warnings"),
                                  T("warnings (many)")));
        if (ImGui::IsItemClicked()) ImGui::SetWindowFocus("Console");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Open console"));
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
        m_panels.Restore();
        m_rebuildDockLayout = true; // панель могла быть закрыта вместе со своим узлом
    }
    ImGui::End();
}

void EditorLayer::DrawDockspaceAndMenu() {
    // Заявки на клавиши — ровно на один кадр, и сбрасываются ДО панелей: иначе
    // забранный вчера Delete молчал бы сегодня (см. HotkeyScope.h).
    sage::editor::hotkeys::NewFrame();

    // Полноэкранное окно-хост под dockspace: без рамок/заголовка, на весь
    // рабочий вьюпорт, с menu bar. Стандартный приём из демо ImGui.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        // ОКНУ-ХОСТУ ПРОКРУТКА НЕ ПОЛОЖЕНА, И БЕЗ ЭТОГО ФЛАГА ОНА ПОЯВЛЯЛАСЬ.
        //
        // Хост — это весь редактор: тулбар, док-пространство и статус-бар,
        // растянутые ровно по окну. Прокручивать его некуда, но ImGui об этом
        // не знает: содержимое на волосок выше окна (полоса статуса плюс
        // отступ строки) — и он заводит вертикальную полосу прокрутки. На
        // экране это серая лента во всю высоту у правого края, отнимающая
        // место у инспектора, причём ездить по ней некуда.
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##SageEditorHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    // Верхняя панель — полоса сразу под меню-баром: Play/Pause/Stop и кнопки
    // окон. Рисуется ДО dockspace, чтобы занять свою полосу.
    //
    // ИНСТРУМЕНТОВ здесь больше нет: гизмо, привязка, показ и раскладка видов
    // переехали виджетом ПОВЕРХ вьюпорта (ViewportTools.cpp) — туда, где ими
    // работают. Раньше за сеткой и шагом привязки мышь ездила от объекта к
    // верхнему краю окна и обратно.
    m_topBar.Draw(*this, TopBarPanel::kHeight);

    // У КАЖДОГО ПРОСТРАНСТВА СВОЁ ДОК-ПРОСТРАНСТВО, и это не деталь
    // реализации. Общее означало бы, что переключение пересобирает раскладку
    // заново — то есть настроенное расположение панелей теряется при каждом
    // переходе «сцена — интерфейс». Разные узлы ImGui запоминает каждый свой,
    // в том же imgui.ini, и обе раскладки переживают перезапуск.
    const bool interfaceSpace = m_workspace == EditorWorkspace::Interface;
    ImGuiID dockspaceId =
        ImGui::GetID(interfaceSpace ? "SageInterfaceDock" : "SageDockSpace");
    // ВТОРОЕ ДОК-ПРОСТРАНСТВО ДЕРЖИМ ЖИВЫМ, хоть и не показываем.
    //
    // Не подать его ImGui значит сказать «этого дока больше нет»: узлы, в
    // которых не осталось ни одного окна, убираются, а окна из неподанного
    // дока выпадают наружу. Именно так панель ассетов и оказывалась висящей
    // посреди экрана: раскладка вёрстки забирала её себе, нижний узел
    // раскладки сцены пустел и исчезал, а вернуться ей было уже некуда.
    // KeepAliveOnly — это ровно «узлы живы, окна свои не отпускают».
    const ImGuiID otherDockspaceId =
        ImGui::GetID(interfaceSpace ? "SageDockSpace" : "SageInterfaceDock");
    if (ImGui::DockBuilderGetNode(otherDockspaceId) != nullptr)
        ImGui::DockSpace(otherDockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_KeepAliveOnly);

    // Куда возвращать панель, у которой сняли галочку «в отдельном окне».
    panelwindows::SetHomeDock(dockspaceId);
    // Строим дефолтную раскладку, если её ещё нет (первый запуск без ini)
    // или пользователь попросил сброс (Window > Reset Layout).
    if (m_rebuildDockLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        m_rebuildDockLayout = false;
        if (interfaceSpace) BuildInterfaceDockLayout(dockspaceId);
        else BuildDefaultDockLayout(dockspaceId);
    }

    // Док-пространство занимает всё между тулбаром и статус-баром.
    const ImVec2 dockMin = ImGui::GetCursorScreenPos();
    // КРЕСТИК У ПАНЕЛИ ОДИН. ImGui рисует их два: свой у каждой вкладки и ещё
    // один в правом углу узла — для вкладки, выбранной сейчас. Второй ничего
    // не добавляет (он закрывает ту же панель, что и первый), а спрашивают о
    // нём как об ошибке: два одинаковых крестика подряд читаются как «один из
    // них закроет что-то другое».
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, -kStatusBarHeight),
                     ImGuiDockNodeFlags_NoCloseButton);
    const ImVec2 dockMax = ImGui::GetItemRectMax();
    DrawStatusBar(kStatusBarHeight);

    // ВАЖНО: OpenPopup нельзя звать изнутри BeginMenu (другой ID-стек — модалка
    // на уровне окна её не найдёт). Меню лишь запоминает, какой диалог открыть;
    // сам OpenPopup зовётся ниже, после EndMenuBar, на уровне окна-хоста.
    const char* openDialog = nullptr;
    if (ImGui::BeginMenuBar()) {
        if (EditorIcons::BeginMenu("file", T("File"))) {
            if (EditorIcons::MenuItem("project", T("New Project..."))) openDialog = "New Project";
            if (EditorIcons::MenuItem("open", T("Open Project..."))) openDialog = "Open Project";
            if (EditorIcons::MenuItem("list", T("Project Launcher..."))) m_launcherRequested = true;
            ImGui::Separator();
            if (EditorIcons::MenuItem("scene", T("New Scene"))) NewSceneForUser();
            if (EditorIcons::MenuItem("open", T("Open Scene..."))) openDialog = "Open Scene";

            // Сцены открытого проекта — прямой доступ без файлового диалога.
            if (EditorIcons::BeginMenu("scene", T("Project Scenes"))) {
                std::error_code ec;
                std::vector<fs::path> scenes;
                for (const auto& entry : fs::directory_iterator(m_project.ScenesDir(), ec)) {
                    if (entry.path().extension() == ".sage") scenes.push_back(entry.path());
                }
                std::sort(scenes.begin(), scenes.end());
                if (scenes.empty()) ImGui::TextDisabled("%s", T("(no scenes yet)"));
                for (const fs::path& scenePath : scenes) {
                    bool current = scenePath == m_scenePath;
                    if (EditorIcons::MenuItemSelected("scene", scenePath.filename().string().c_str(),
                                                      current)) {
                        LoadSceneFromFile(scenePath);
                    }
                }
                ImGui::EndMenu();
            }

            if (EditorIcons::MenuItem("save", T("Save Scene"), "Ctrl+S")) SaveCurrentScene();
            if (EditorIcons::MenuItem("save", T("Save Scene As..."))) openDialog = "Save Scene As";
            ImGui::Separator();
            if (EditorIcons::MenuItem("build", T("Build Game..."))) {
                openDialog = "Build Game";
            }
            ImGui::Separator();
            if (EditorIcons::MenuItem("exit", T("Exit"))) {
                m_closeAfterPrompt = true;
                AskUnsaved(nullptr);
                if (!m_unsavedPrompt) sage::Application::Get().Close();
            }
            ImGui::EndMenu();
        }
        if (EditorIcons::BeginMenu("pencil", T("Edit"))) {
            if (EditorIcons::MenuItem("undo", T("Undo"), "Ctrl+Z",
                                      m_history.CanUndo() && !InPlayMode())) Undo();
            if (EditorIcons::MenuItem("redo", T("Redo"), "Ctrl+Y",
                                      m_history.CanRedo() && !InPlayMode())) Redo();
            ImGui::Separator();
            bool hasSel = m_scene->Get(m_selection.Primary()).Valid();
            if (EditorIcons::MenuItem("copy", T("Duplicate"), "Ctrl+D", hasSel)) DuplicateSelected();
            if (EditorIcons::MenuItem("trash", T("Delete"), "Del", hasSel)) DeleteSelected();
            ImGui::EndMenu();
        }
        if (EditorIcons::BeginMenu("play", T("Play"))) {
            if (EditorIcons::MenuItem("play", T("Play"), nullptr, !m_play.Active())) StartPlay();
            if (EditorIcons::MenuItem("pause", T("Pause"), nullptr, m_play.Playing())) PausePlay();
            if (EditorIcons::MenuItem("play", T("Resume"), nullptr, m_play.Paused())) ResumePlay();
            if (EditorIcons::MenuItem("stop", T("Stop"), nullptr, InPlayMode())) StopPlay();
            ImGui::EndMenu();
        }
        // ОБЪЕКТ — ОДИН КАТАЛОГ НА ВЕСЬ РЕДАКТОР.
        //
        // Здесь был свой список пунктов, а под правой кнопкой в иерархии —
        // свой, вдвое короче. Теперь оба рисуют общий каталог
        // (editor/src/ObjectCatalog.h), и разойтись им нечем.
        if (EditorIcons::BeginMenu("cube", T("Object"))) {
            if (const char* pick = sage::editor::objectcatalog::DrawMenu()) CreateCatalogObject(pick);
            ImGui::EndMenu();
        }
        if (EditorIcons::BeginMenu("layout", T("Window"))) {
            // --- МЕНЮ РАЗБИТО ПО СМЫСЛУ, А НЕ ВЫСЫПАНО СПИСКОМ --------------
            //
            // Панелей полтора десятка, и плоским столбцом они читаются как
            // случайный набор: рядом стояли «Иерархия» и «Элементы» — почти
            // одинаковые слова про совершенно разные вещи (объекты сцены и
            // элементы интерфейса), — а между ними «Ассеты» и «Консоль»,
            // которые есть и там и там. Найти нужное можно было только прочитав
            // все пятнадцать строк подряд.
            //
            // Разделы — те же, что и рабочие места редактора: сцена, интерфейс,
            // общие панели, инструменты. Человек ищет окно в том разделе, в
            // котором он сейчас работает, и читает четыре строки вместо
            // пятнадцати.
            Sage::UI::MenuSection(T("Scene"), true);
            EditorIcons::MenuItemToggle("world", T("Viewport"), &PanelVisible(EditorPanel::Viewport));
            EditorIcons::MenuItemToggle("game", T("Game"), &PanelVisible(EditorPanel::Game));
            EditorIcons::MenuItemToggle("hierarchy", T("Hierarchy"), &PanelVisible(EditorPanel::Hierarchy));
            EditorIcons::MenuItemToggle("inspector", T("Inspector"), &PanelVisible(EditorPanel::Inspector));
            // «Освещение» стало «Средой»: в окне остались небо, воздух и
            // окружающий свет, а сами источники света — на объектах сцены.
            EditorIcons::MenuItemToggle("sun", T("Environment"), &PanelVisible(EditorPanel::Environment));

            Sage::UI::MenuSection(T("Interface"));
            EditorIcons::MenuItemToggle("layout", T("Canvas"), &PanelVisible(EditorPanel::InterfaceViewport));
            EditorIcons::MenuItemToggle("list", T("Elements"), &PanelVisible(EditorPanel::InterfaceHierarchy));
            EditorIcons::MenuItemToggle("inspector", T("Element"), &PanelVisible(EditorPanel::InterfaceInspector));
            EditorIcons::MenuItemToggle("eye", T("Preview"), &PanelVisible(EditorPanel::InterfacePreview));

            // Эти трое стоят в ОБОИХ рабочих местах — потому и отдельным
            // разделом, а не в одном из двух выше.
            Sage::UI::MenuSection(T("Shared"));
            EditorIcons::MenuItemToggle("folder-full", T("Assets"), &PanelVisible(EditorPanel::Assets));
            EditorIcons::MenuItemToggle("console", T("Console"), &PanelVisible(EditorPanel::Console));
            EditorIcons::MenuItemToggle("anim", T("Animation"), &PanelVisible(EditorPanel::Animation));

            // Инструменты открывают ПОД ЗАДАЧУ и закрывают: они не участвуют в
            // раскладке по умолчанию и не живут на экране постоянно.
            Sage::UI::MenuSection(T("Tools"));
            EditorIcons::MenuItemToggle("chart", T("Profiler"), &PanelVisible(EditorPanel::Profiler));
            EditorIcons::MenuItemToggle("nineslice", T("9-slice editor"), &m_showNineSlice);
            EditorIcons::MenuItemToggle("grid", T("Icon sheet"), &m_showIconSheet);
            // --- РАСКЛАДКА: то, что относится к самому окну редактора -------
            //
            // Внизу, а не первой строкой, как было: «сбросить раскладку»
            // нажимают раз в месяц, а стояло оно там, куда взгляд падает
            // первым, — перед списком окон, ради которого меню и открывают.
            Sage::UI::MenuSection(T("Layout"));
            // Сброс раскладки возвращает и сами панели: закрытая вкладка иначе
            // не восстанавливалась «сбросом», хотя именно этого от него ждут.
            if (EditorIcons::MenuItem("refresh", T("Reset Layout"))) {
                m_panels.Restore();
                m_rebuildDockLayout = true;
            }
            EditorIcons::MenuItemToggle("grid", T("Show Grid"), &m_tools.ShowGrid);

            // --- Панель ОТДЕЛЬНЫМ ОКНОМ СИСТЕМЫ -----------------------------
            //
            // Меню, а не только перетаскивание мышью: вытащить панель в своё
            // окно раньше можно было единственным жестом — дотащить её за
            // вкладку за край главного окна, — а вернуть обратно нечем, кроме
            // сброса всей раскладки. И главное, вытащенное окно слипалось с
            // главным обратно, стоило его туда надвинуть (см. PanelWindows.h).
            if (EditorIcons::BeginMenu("window", T("Separate window"))) {
                ImGui::TextDisabled("%s", T("Panel becomes a window of the system"));
                ImGui::Separator();
                struct DetachRow { const char* Id; const char* Label; EditorPanel Panel; };
                static const DetachRow kRows[] = {
                    {"InterfaceViewport",  "Canvas",   EditorPanel::InterfaceViewport},
                    {"InterfaceHierarchy", "Elements", EditorPanel::InterfaceHierarchy},
                    {"InterfaceInspector", "Element",  EditorPanel::InterfaceInspector},
                    {"Viewport",  "Viewport",    EditorPanel::Viewport},
                    {"Game",      "Game",        EditorPanel::Game},
                    {"Hierarchy", "Hierarchy",   EditorPanel::Hierarchy},
                    {"Inspector", "Inspector",   EditorPanel::Inspector},
                    {"Lighting",  "Environment", EditorPanel::Environment},
                    {"Assets",    "Assets",      EditorPanel::Assets},
                    {"Console",   "Console",     EditorPanel::Console},
                    {"Animation", "Animation",   EditorPanel::Animation},
                    // Окно у панели своё в каждом пространстве, а галочка
                    // «отдельным окном» — одна: это свойство ПАНЕЛИ, а не
                    // места, где она сейчас стоит (см. PanelWindowId.h).
                };
                for (const DetachRow& row : kRows) {
                    // Имя окна — ТЕКУЩЕГО пространства: у общих панелей их два,
                    // и галочка обязана относиться к тому окну, которое человек
                    // сейчас видит.
                    const std::string id = sage::editor::panelid::For(row.Id, m_workspace);
                    const bool detached = panelwindows::Detached(id.c_str());
                    if (EditorIcons::MenuItemSelected("window", T(row.Label), detached)) {
                        panelwindows::SetDetached(id.c_str(), !detached);
                        // Отдельное окно ЗАКРЫТОЙ панели — это окно, которого
                        // не видно: галочка стоит, а на экране ничего не
                        // изменилось. Поэтому отрыв открывает панель заодно.
                        if (!detached) PanelVisible(row.Panel) = true;
                    }
                }
                ImGui::Separator();
                // Путь назад для случая «окно уехало на второй монитор, а
                // монитора больше нет»: оттуда его не достать ни мышью, ни
                // галочкой — окна не видно, чтобы за него взяться.
                if (EditorIcons::MenuItem("layout", T("Bring all windows back")))
                    panelwindows::AttachAll();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            EditorIcons::MenuItemToggle("gear", T("Game Settings..."), &PanelVisible(EditorPanel::Settings));
            // Управление — рядом с настройками игры: это тоже содержимое
            // проекта, которое уезжает в собранную игру, а не настройка
            // редактора.
            EditorIcons::MenuItemToggle("keyboard", T("Controls..."), &PanelVisible(EditorPanel::Input));
            // Шаблоны — рядом с настройками игры и языком, потому что это
            // настройка ОКРУЖЕНИЯ, а не текущей сцены: что установлено у меня
            // на машине и откуда это брать.
            EditorIcons::MenuItemToggle("template", T("Project templates..."), &m_showTemplates);

            // Язык интерфейса. Здесь, а не в окне Settings: то окно правит
            // настройки ИГРЫ и сохраняется в проект, а язык — настройка
            // редактора и живёт в профиле пользователя. Смешать их значило бы
            // получить проект, навязывающий язык всем, кто его откроет.
            // Оформление — рядом с языком и по той же причине: это настройка
            // РЕДАКТОРА, она живёт в профиле человека, а не в проекте. Проект,
            // навязывающий свою тему каждому, кто его открыл, — не то, чего
            // ждут от проекта.
            if (EditorIcons::BeginMenu("theme", T("Appearance"))) {
                for (const EditorTheme::Theme& theme : EditorTheme::Themes()) {
                    const bool active = theme.Id == EditorTheme::CurrentId();
                    // Название темы не переводится — как и название языка: по
                    // нему тему находят в файле настроек и в themes/*.json.
                    if (EditorIcons::MenuItemSelected("theme", theme.Name.c_str(), active) && !active) {
                        EditorTheme::SetTheme(theme.Id);
                        SetStatusMessage(T("Theme changed"));
                    }
                }
                ImGui::Separator();
                // Масштаб интерфейса — здесь же: на экране 4K шрифт в 16 точек
                // читается лупой, и «сделайте покрупнее» до сих пор решалось
                // только сменой разрешения всей системы.
                ImGui::TextDisabled("%s", T("Interface scale"));
                float scale = EditorTheme::UiScale();
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::SliderFloat("##uiscale", &scale, 0.75f, 2.0f, "%.2fx"))
                    EditorTheme::SetUiScale(scale);
                if (EditorIcons::MenuItem("refresh", T("Reset scale"), nullptr, scale != 1.0f))
                    EditorTheme::SetUiScale(1.0f);
                ImGui::Separator();
                // Выгрузка темы файлом — вот ради чего вся система: взять
                // текущую за основу, поправить цвета в текстовом редакторе и
                // положить обратно в themes/. Пересборка не нужна.
                if (EditorIcons::MenuItem("save", T("Export theme to themes/..."))) {
                    const std::string dir = EditorTheme::ThemesDir();
                    const std::string path = dir + "/" + EditorTheme::Current().Id + "-copy.json";
                    if (EditorTheme::ExportTheme(EditorTheme::Current(), path))
                        SetStatusMessage(std::string(T("Theme saved:")) + " " + path);
                    else
                        SetStatusMessage(T("Could not write the theme file"));
                }
                ImGui::EndMenu();
            }
            // Чем открывать код. Здесь же, где язык и оформление, и по той же
            // причине: это настройка РЕДАКТОРА, она живёт в профиле человека.
            // Проект, навязывающий соавтору свою IDE, — не то, чего ждут от
            // проекта.
            if (EditorIcons::BeginMenu("code", T("Code editor"))) {
                namespace codeapp = sage::editor::codeapp;
                const std::string current = codeapp::CurrentId();
                // Системная ассоциация — всегда первая и всегда доступна: это
                // единственный вариант, который не может «не установиться».
                if (EditorIcons::MenuItemSelected("gear", T("As the system opens it"), current.empty()))
                    codeapp::SetCurrent("");
                const std::vector<codeapp::App>& apps = codeapp::Available();
                if (!apps.empty()) ImGui::Separator();
                for (const codeapp::App& app : apps) {
                    // Имя программы НЕ переводится — как название темы и языка:
                    // по нему её узнают в меню «Пуск» и в своей же панели задач.
                    if (EditorIcons::MenuItemSelected("code", app.Name.c_str(), app.Id == current))
                        codeapp::SetCurrent(app.Id);
                }
                ImGui::Separator();
                if (apps.empty()) {
                    // Пустой список — не поломка, и сказать об этом надо прямо:
                    // иначе меню из одного пункта читается как «выбор сломан».
                    ImGui::TextDisabled("%s", T("No code editors found on this machine"));
                }
                ImGui::TextDisabled("%s", T("Only what is actually installed is listed"));
                if (EditorIcons::MenuItem("search", T("Search again"))) {
                    codeapp::Rescan();
                    SetStatusMessage(T("Code editors: ") +
                                     std::to_string(codeapp::Available().size()));
                }
                ImGui::EndMenu();
            }
            if (EditorIcons::BeginMenu("language", T("Language"))) {
                for (const sage::editor::LanguageInfo& lang : sage::editor::AvailableLanguages()) {
                    const bool active = lang.Code == sage::editor::CurrentLanguageCode();
                    // Название языка НЕ переводится: человек, случайно
                    // переключивший интерфейс на незнакомый, должен найти
                    // дорогу назад по слову «English», а не по переводу.
                    if (EditorIcons::MenuItemSelected("language", lang.Name.c_str(), active) && !active) {
                        sage::editor::SetLanguage(lang.Code);
                        SetStatusMessage(T("Interface language changed"));
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (EditorIcons::BeginMenu("question", T("Help"))) {
            EditorIcons::MenuItemToggle("info", T("About SAGE..."), &m_showAbout);
            ImGui::EndMenu();
        }

        // Статус проекта справа в меню-баре.
        std::string status = std::string(T("Project:")) + " " + m_project.Name();
        float w = ImGui::CalcTextSize(status.c_str()).x + 16.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - w);
        ImGui::TextDisabled("%s", status.c_str());

        ImGui::EndMenuBar();
    }

    // Модалки диалогов и окно настроек — самостоятельные панели, но рисуются
    // ЗДЕСЬ, на уровне окна-хоста: модалка ImGui совпадает с OpenPopup по
    // ID-стеку окна, поэтому открытие и отрисовку нельзя разносить по окнам.
    // Открыть диалог по имени из переменной окружения — тем же путём, каким его
    // открывает пункт меню. Нужно затем же, зачем SAGE_EDITOR_ICON_SHEET:
    // модалку нельзя увидеть headless, а значит, нечем проверить, как она
    // выглядит; открывалась она только мышью.
    // Не в первом кадре: на первых кадрах редактор перестраивает раскладку
    // доков, а это фокусирует окна панелей — и ImGui закрывает всплывающие
    // окна, открытые над окном-хостом. Модалка, открытая в кадре 1, исчезала
    // ровно поэтому, и на скриншоте её не было.
    if (!openDialog && m_pendingDialog && m_frameCounter > 4) {
        openDialog = m_pendingDialog;
        m_pendingDialog = nullptr;
    }
    if (openDialog) m_dialogs.Open(openDialog);
    m_dialogs.Draw(*this);
    // Отчёт о падении — ПЕРЕД предложением восстановить сцену: сначала «что
    // случилось», потом «что с этим делать».
    DrawCrashReport();
    DrawRecoveryPrompt();
    // Вопрос о несохранённой сцене. Сюда он попадает и от пункта «Выход», и от
    // крестика окна — тот приходит через EditorLayer::OnCloseRequest (см.
    // Layer.h): проверять флаг GLFW прямо здесь бесполезно, цикл выходит
    // раньше, чем этот код выполнится.
    DrawUnsavedPrompt();
    m_settingsPanel.Draw(*this, m_panels[EditorPanel::Settings]);
    m_inputPanel.Draw(*this, m_panels[EditorPanel::Input]);
    m_assets.Tick(*this);           // пакетная конвертация идёт по кадрам, а не одним куском
    m_templatesPanel.Tick(*this);   // фоновая загрузка шаблона доводится до конца и с закрытым окном
    m_templatesPanel.Draw(*this, m_showTemplates);
    m_nineSlice.Draw(*this, m_showNineSlice);
    {
        // Инструмент анимации — тоже в обоих пространствах, тоже своим окном.
        const std::string id = sage::editor::panelid::For("Animation", m_workspace);
        m_animation.Draw(*this, &m_panels[EditorPanel::Animation], id);
    }
    // Профилировщик — через ту же обёртку, что и остальные панели: он тоже
    // имеет право жить своим окном системы, и панель, которую забыли обернуть,
    // молча теряет это право (см. PanelWindows.h).
    if (m_panels[EditorPanel::Profiler]) {
        panelwindows::Before("Profiler");
        m_profiler.Draw(&m_panels[EditorPanel::Profiler]);
        panelwindows::After("Profiler");
    }
    if (m_showIconSheet) EditorIcons::DrawSheet(&m_showIconSheet);
    m_confirm.Draw();
    DrawAboutWindow();

    ImGui::End();

    // Все панели закрыты — на месте редактора пустой прямоугольник. Молчать
    // здесь нельзя: человек видит серое поле и не знает, что перед ним
    // результат его же крестиков, а не поломка. Подсказка рисуется ПОСЛЕ
    // окна-хоста и отдельным окном: внутри хоста её накрывает фон пустого
    // док-узла, который ImGui кладёт в фоновый канал списка отрисовки.
    if (!m_panels.AnyVisible()) DrawEmptyDockHint(dockMin.x, dockMin.y, dockMax.x, dockMax.y);

    // Глобальные хоткеи (когда не печатаем в поле ввода).
    ImGuiIO& io = ImGui::GetIO();
    // Палитра команд — ДО проверки на ввод текста: Ctrl+K обязан открывать её
    // и из поля поиска ассетов, иначе «горячая клавиша, которая иногда не
    // работает» хуже отсутствующей.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) m_palette.Open(m_paletteQuery);
    m_palette.Draw(m_commands);

    if (!io.WantTextInput) {
        // Delete — только если его не забрала панель со своим смыслом этой
        // клавиши (анимация убирает ключ, ассеты — файл, список элементов —
        // элемент). Без этой проверки срабатывали ОБА: человек убирал ключ в
        // линейке времени и вместе с ним терял объект из сцены (HotkeyScope.h).
        namespace hk = sage::editor::hotkeys;
        if (!hk::Claimed(hk::Key::Delete) && ImGui::IsKeyPressed(ImGuiKey_Delete))
            DeleteSelected();
        if (!hk::Claimed(hk::Key::Duplicate) && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D))
            DuplicateSelected();
        // Ctrl+S работает ВСЕГДА, а не только у сцены с именем: у новой сцены
        // имени нет, и «горячая клавиша молча ничего не делает» — это ровно то,
        // как выглядит потерянная работа.
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) SaveCurrentScene();
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
        if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
            (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))) Redo();
    }
}

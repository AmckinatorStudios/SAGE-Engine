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

#include "EditorTheme.h"
#include "ui/UI.h"
#include "sage/core/Profiler.h"
#include "EditorIcons.h"
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
#include "sage/scene/Prefab.h"
#include "sage/scene/SceneSerializer.h"
#include "Localization.h"

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

// Высота статусной строки — ОТ ТОКЕНОВ, а не числом.
//
// Числом она была 26 точек при любом масштабе интерфейса: при 150% строка
// оставалась той же, а текст в ней вырастал — и обрезался снизу примерно на
// треть. Видно это только на экране с другим DPI, поэтому и держалось долго.
float StatusBarHeight() { return Sage::UI::Get().StatusBarHeight; }

} // namespace


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

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    // Панель вёрстки — вкладкой к иерархии слева, а не плавающим окном. Плавать
    // ей нельзя: она открывается сама вместе с режимом вёрстки и накрывала бы
    // собой ровно тот вьюпорт, ради которого её и открыли.
    ImGui::DockBuilderDockWindow("Lighting", right);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    // Viewport докается ПЕРВЫМ в центральный узел — так он и есть таб по
    // умолчанию (первый добавленный к узлу становится выбранным). Раньше первым
    // шёл Game, из-за чего редактор открывался на «игровом окне» без пикинга/
    // гизмо/аутлайна — выглядело как «выделение не работает». Game выходит
    // вперёд при входе в Play (GamePanel::RequestFocus).
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Game", center);
    // Код — третьей вкладкой рядом с Viewport и Game, а не отдельным плавающим
    // окном. Правка скрипта и проверка результата — это одно занятие, и
    // «редактор кода поверх сцены» заставлял таскать окно с места на место
    // ровно так же, как раньше заставлял alt-tab во внешний редактор.
    ImGui::DockBuilderDockWindow("Code", center);
    // Редактор интерфейса — вкладкой в центр, к вьюпорту и игре: это
    // такой же основной рабочий вид, только для другого предмета работы.
    ImGui::DockBuilderDockWindow("UIEditor", center);
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
    // Play-статус | сообщение плагинов | справа показатели сцены и версия.
    //
    // ЛЕВАЯ ЧАСТЬ СЧИТАЕТСЯ ПОД УЖЕ ИЗВЕСТНУЮ ПРАВУЮ, а не наоборот. Правая
    // умела ужиматься с самого начала, левая — нет, и на 1280 точках они
    // сталкивались посреди строки: «1 ошибка, 3 предупреждениПамять 0.30 ГБ».
    // Наезжали именно те два места, ради которых строка и существует —
    // счётчик ошибок и расход памяти.
    //
    // Что уходит первым, решает польза, а не порядок: имя проекта и сцены
    // остаются всегда, счётчик ошибок и состояние игры — почти всегда (это
    // тревога, её нельзя прятать первой), а «объектов в сцене» и сообщение
    // плагина уступают место, потому что то же самое есть в других местах
    // экрана.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 3));
    ImGui::BeginChild("##statusbar", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::AlignTextToFramePadding();

    const Sage::UI::Style& ui = Sage::UI::Get();
    const float gap = ui.SpacingLG;

    // --- СПРАВА: показатели сцены и версия движка (считаем ширину заранее) ---
    //
    // Пары «подпись — значение», а не слитная строка «meshes 8/8 culled 0
    // batches 4»: слитную читают только те, кто её писал. Подпись приглушена,
    // значение обычным цветом — глаз находит число, не читая всю строку.
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
    float need = ImGui::CalcTextSize(version.c_str()).x + ui.IconSize + gap;
    int shown = 0;
    for (int i = (int)IM_ARRAYSIZE(stats) - 1; i >= 0; --i) {
        const float w = ImGui::CalcTextSize(stats[i].Label).x +
                        ImGui::CalcTextSize(stats[i].Value).x + ui.SpacingSM + gap;
        if (need + w > ImGui::GetWindowWidth() * 0.55f) break;
        need += w;
        ++shown;
    }
    // Предел для левой части: до правой должен остаться зазор, иначе строки
    // сойдутся вплотную и прочитаются как одна.
    const float leftLimit = ImGui::GetWindowWidth() - need - gap;

    // --- СЛЕВА -------------------------------------------------------------
    const float sepW = ImGui::CalcTextSize("|").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    // Влезает ли ещё кусок такой ширины (вместе с разделителем перед ним).
    //
    // Мерится правый край ПОСЛЕДНЕГО нарисованного элемента, а не
    // GetCursorPosX(): элемент, закрывший строку, оставляет курсор на
    // следующей строке и её левом краю — то есть GetCursorPosX() отвечает
    // «места сколько угодно» ровно тогда, когда его нет.
    auto room = [&](float w) {
        const float used = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        return used + sepW + w <= leftLimit;
    };
    auto sep = [&]() { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); };

    // Точка состояния: зелёная в покое, жёлтая в игре. Один символ, но он
    // отвечает на вопрос «редактор жив?» без чтения строки.
    {
        const bool busy = InPlayMode();
        const ImVec4 dot = EditorTheme::Color(busy ? EditorTheme::Role::Accent
                                                   : EditorTheme::Role::Ok);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float r = ui.SpacingXS * 0.75f;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f), r,
            ImGui::GetColorU32(dot));
        ImGui::Dummy(ImVec2(r * 2.0f, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0.0f, ui.SpacingSM);
    }

    ImGui::TextDisabled("%s", m_project.Name().c_str());
    sep();
    std::string scene = m_scenePath.empty() ? m_scene->Name() : m_scenePath.filename().string();
    // Имя сцены обрезается, а не выталкивает соседей: длинное имя файла — не
    // повод потерять счётчик ошибок.
    const std::string sceneShown =
        Sage::UI::Truncate((scene + (m_sceneDirty ? "*" : "")).c_str(),
                           std::max(ui.RowHeight * 3.0f, leftLimit * 0.35f));
    ImGui::TextUnformatted(sceneShown.c_str());
    if (ImGui::IsItemHovered() && sceneShown != scene) ImGui::SetTooltip("%s", scene.c_str());

    // Ошибки и предупреждения — В СТАТУСНОЙ СТРОКЕ, а не только в консоли.
    // Консоль легко держать свёрнутой, и тогда единственное предупреждение о
    // непрочитанном шейдере остаётся незамеченным, а сцена «просто выглядит
    // не так». Клик открывает консоль уже с нужным фильтром.
    if (m_console.ErrorCount() > 0 || m_console.WarnCount() > 0) {
        const bool hasErrors = m_console.ErrorCount() > 0;
        const ImVec4 col = hasErrors ? EditorTheme::Color(EditorTheme::Role::Danger)
                                     : EditorTheme::Color(EditorTheme::Role::Warn);
        // Склонение по-русски: 1 ошибка, 2 ошибки, 5 ошибок. Мелочь, но
        // «1 ошибок» в статусной строке читается как недоделка интерфейса.
        auto plural = [](int n, const char* one, const char* few, const char* many) {
            const int n100 = n % 100, n10 = n % 10;
            if (n100 >= 11 && n100 <= 14) return many;
            if (n10 == 1) return one;
            if (n10 >= 2 && n10 <= 4) return few;
            return many;
        };
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%d %s, %d %s", m_console.ErrorCount(),
                      plural(m_console.ErrorCount(), T("error"), T("errors"), T("errors (many)")),
                      m_console.WarnCount(),
                      plural(m_console.WarnCount(), T("warning"), T("warnings"),
                             T("warnings (many)")));
        const float w = ImGui::CalcTextSize(msg).x + ui.IconSize + ui.SpacingXS;
        // Когда даже это не влезает, остаётся один значок с числом ошибок:
        // молча убрать тревогу нельзя, а места под слова может не быть.
        char shortMsg[32];
        std::snprintf(shortMsg, sizeof(shortMsg), "%d", m_console.ErrorCount() + m_console.WarnCount());
        const float shortW = ImGui::CalcTextSize(shortMsg).x + ui.IconSize + ui.SpacingXS;
        const bool full = room(w);
        if (full || room(shortW)) {
            sep();
            EditorIcons::Inline(hasErrors ? "error" : "warn", glm::vec3(col.x, col.y, col.z));
            ImGui::SameLine();
            ImGui::TextColored(col, "%s", full ? msg : shortMsg);
            if (ImGui::IsItemClicked()) ImGui::SetWindowFocus("Console");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", full ? T("Open console") : msg);
        }
    }

    if (InPlayMode()) {
        const bool playing = m_playState == EditorPlayState::Playing;
        const char* label = playing ? "PLAYING" : "PAUSED";  // no-i18n: состояние движка
        if (room(ImGui::CalcTextSize(label).x)) {
            sep();
            ImGui::TextColored(EditorTheme::Color(playing ? EditorTheme::Role::Ok
                                                          : EditorTheme::Role::Warn),
                               "%s", label);
        }
    }

    // Ниже — то, что уступает место первым: число сущностей повторяет счётчик
    // объектов справа, а сообщение плагина живёт секунды и дублируется логом.
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), T("Entities: %zu"), m_scene->Count());
        if (room(ImGui::CalcTextSize(buf).x)) { sep(); ImGui::TextDisabled("%s", buf); }
    }
    if (!m_pluginStatusMessage.empty() &&
        room(ImGui::CalcTextSize(m_pluginStatusMessage.c_str()).x)) {
        sep();
        ImGui::TextDisabled("%s", m_pluginStatusMessage.c_str());
    }

    // --- СПРАВА: рисуем то, что посчитали выше -----------------------------
    ImGui::SameLine(ImGui::GetWindowWidth() - need);
    for (int i = (int)IM_ARRAYSIZE(stats) - shown; i < (int)IM_ARRAYSIZE(stats); ++i) {
        ImGui::TextDisabled("%s", stats[i].Label);
        ImGui::SameLine(0.0f, ui.SpacingSM);
        ImGui::TextUnformatted(stats[i].Value);
        ImGui::SameLine(0.0f, gap);
    }
    {
        const ImVec4 a = EditorTheme::Color(EditorTheme::Role::Accent);
        EditorIcons::Inline("cube", glm::vec3(a.x, a.y, a.z));
    }
    ImGui::SameLine(0.0f, ui.SpacingXS);
    ImGui::TextDisabled("%s", version.c_str());

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

bool EditorLayer::AnyPanelVisible() const {
    return m_showHierarchy || m_showInspector || m_showEnvironment || m_showUIEditor ||
           m_showViewport || m_showGame ||
           m_showConsole || m_showAssets || m_showCode || m_showProfiler;
}

void EditorLayer::ShowAllPanels() {
    m_showHierarchy = m_showInspector = m_showEnvironment = true;
    // Панель вёрстки в «показать все» НЕ входит: она инструмент под задачу, а
    // не часть постоянной раскладки, и открывать её вместе со всем остальным
    // значит отдать ей место у человека, который сейчас собирает сцену.
    m_showViewport = m_showGame = m_showConsole = m_showAssets = true;
    m_showCode = true;
    // Профайлер сюда НЕ входит: он служебный и по умолчанию закрыт, а «вернуть
    // панели» не должно означать «открыть то, чего человек не открывал».
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
        ShowAllPanels();
        m_rebuildDockLayout = true; // панель могла быть закрыта вместе со своим узлом
    }
    ImGui::End();
}

void EditorLayer::DrawDockspaceAndMenu() {
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
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
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

    ImGuiID dockspaceId = ImGui::GetID("SageDockSpace");
    // Строим дефолтную раскладку, если её ещё нет (первый запуск без ini)
    // или пользователь попросил сброс (Window > Reset Layout).
    if (m_rebuildDockLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        m_rebuildDockLayout = false;
        BuildDefaultDockLayout(dockspaceId);
    }
    // Док-пространство занимает всё между тулбаром и статус-баром.
    const ImVec2 dockMin = ImGui::GetCursorScreenPos();
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, -StatusBarHeight()), ImGuiDockNodeFlags_None);
    const ImVec2 dockMax = ImGui::GetItemRectMax();
    DrawStatusBar(StatusBarHeight());

    // ВАЖНО: OpenPopup нельзя звать изнутри BeginMenu (другой ID-стек — модалка
    // на уровне окна её не найдёт). Меню лишь запоминает, какой диалог открыть;
    // сам OpenPopup зовётся ниже, после EndMenuBar, на уровне окна-хоста.
    const char* openDialog = nullptr;
    if (ImGui::BeginMenuBar()) {
        // ЗНАК ПРОДУКТА ПЕРЕД МЕНЮ. Строка меню начиналась прямо со слова
        // «Файл» — так выглядит окно утилиты, а не редактора движка. Имя и знак
        // стоят там, где их ищут: в самом левом верхнем углу.
        {
            const Sage::UI::Style& ui = Sage::UI::Get();
            ImGui::Dummy(ImVec2(ui.SpacingSM, 0.0f));
            ImGui::SameLine(0.0f, 0.0f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            EditorIcons::Overlay(p.x, p.y + (ImGui::GetFrameHeight() - ui.IconSize) * 0.5f,
                                 ui.IconSize, "cube",
                                 glm::vec3(EditorTheme::Color(EditorTheme::Role::Accent).x,
                                           EditorTheme::Color(EditorTheme::Role::Accent).y,
                                           EditorTheme::Color(EditorTheme::Role::Accent).z));
            ImGui::Dummy(ImVec2(ui.IconSize + ui.SpacingXS, ImGui::GetFrameHeight()));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("SAGE");  // no-i18n: имя движка
            ImGui::SameLine(0.0f, ui.SpacingMD);
        }

        if (ImGui::BeginMenu(T("File"))) {
            if (ImGui::MenuItem(T("New Project..."))) openDialog = "New Project";
            if (ImGui::MenuItem(T("Open Project..."))) openDialog = "Open Project";
            if (ImGui::MenuItem(T("Project Launcher..."))) m_launcherRequested = true;
            ImGui::Separator();
            if (ImGui::MenuItem(T("New Scene"))) NewScene(ProjectTemplateKind::Empty);
            if (ImGui::MenuItem(T("Open Scene..."))) openDialog = "Open Scene";

            // Сцены открытого проекта — прямой доступ без файлового диалога.
            if (ImGui::BeginMenu(T("Project Scenes"))) {
                std::error_code ec;
                std::vector<fs::path> scenes;
                for (const auto& entry : fs::directory_iterator(m_project.ScenesDir(), ec)) {
                    if (entry.path().extension() == ".sage") scenes.push_back(entry.path());
                }
                std::sort(scenes.begin(), scenes.end());
                if (scenes.empty()) ImGui::TextDisabled("%s", T("(no scenes yet)"));
                for (const fs::path& scenePath : scenes) {
                    bool current = scenePath == m_scenePath;
                    if (ImGui::MenuItem(scenePath.filename().string().c_str(), nullptr, current)) {
                        LoadSceneFromFile(scenePath);
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem(T("Save Scene"), "Ctrl+S")) {
                if (!m_scenePath.empty()) SaveSceneToFile(m_scenePath);
                else openDialog = "Save Scene As";
            }
            if (ImGui::MenuItem(T("Save Scene As..."))) openDialog = "Save Scene As";
            ImGui::Separator();
            if (ImGui::MenuItem(T("Build Game..."))) {
                openDialog = "Build Game";
            }
            ImGui::Separator();
            if (ImGui::MenuItem(T("Exit"))) sage::Application::Get().Close();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Edit"))) {
            if (ImGui::MenuItem(T("Undo"), "Ctrl+Z", false, !m_undoStack.empty() && !InPlayMode())) Undo();
            if (ImGui::MenuItem(T("Redo"), "Ctrl+Y", false, !m_redoStack.empty() && !InPlayMode())) Redo();
            ImGui::Separator();
            bool hasSel = m_scene->Get(m_selectedId).Valid();
            if (ImGui::MenuItem(T("Duplicate"), "Ctrl+D", false, hasSel)) DuplicateSelected();
            if (ImGui::MenuItem(T("Delete"), "Del", false, hasSel)) DeleteSelected();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Play"))) {
            if (ImGui::MenuItem(T("Play"), nullptr, false, m_playState == EditorPlayState::Editing)) StartPlay();
            if (ImGui::MenuItem(T("Pause"), nullptr, false, m_playState == EditorPlayState::Playing)) PausePlay();
            if (ImGui::MenuItem(T("Resume"), nullptr, false, m_playState == EditorPlayState::Paused)) ResumePlay();
            if (ImGui::MenuItem(T("Stop"), nullptr, false, InPlayMode())) StopPlay();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Entity"))) {
            if (ImGui::MenuItem(T("Create Empty"))) {
                PushUndoSnapshot();
                SetSelectedId(m_scene->CreateObject("Empty").Id());
            }
            if (ImGui::BeginMenu(T("Create Primitive"))) {
                struct { const char* name; MeshRef::Type type; } prims[] = {
                    {"Cube", MeshRef::Type::Cube}, {"Sphere", MeshRef::Type::Sphere},
                    {"Plane", MeshRef::Type::Plane}, {"Cylinder", MeshRef::Type::Cylinder},
                    {"Cone", MeshRef::Type::Cone},
                };
                for (const auto& p : prims) {
                    if (ImGui::MenuItem(p.name)) {
                        PushUndoSnapshot();
                        SetSelectedId(CreatePrimitiveEntity(p.name, p.type).Id());
                    }
                }
                ImGui::EndMenu();
            }
            // Наклейка ставится в СЕРЕДИНУ вида и смотрит туда же, куда
            // камера: поставленная в начало координат, она чаще всего оказалась
            // бы внутри пола или далеко за спиной, и первое, что пришлось бы
            // делать, — искать её.
            if (ImGui::MenuItem(T("Create Decal"))) {
                PushUndoSnapshot();
                GameObject d = m_scene->CreateObject("Decal");
                d.Renderer().Ref = MeshRef{MeshRef::Type::None, ""};
                d.GetTransform().Position = m_camera.Position + m_camera.Front * 4.0f;
                // Ось Z наклейки — навстречу камере: проекция идёт вдоль -Z, то
                // есть от зрителя вглубь сцены, как и смотрит человек.
                const glm::vec3 f = -m_camera.Front;
                d.GetTransform().Rotation =
                    glm::vec3(glm::degrees(std::asin(glm::clamp(f.y, -1.0f, 1.0f))),
                              glm::degrees(std::atan2(f.x, f.z)), 0.0f);
                d.GetTransform().Scale = glm::vec3(1.0f);
                m_scene->Registry().emplace<DecalComponent>(d.Entity());
                SetSelectedId(d.Id());
            }

            // Готовые элементы интерфейса, а не «добавь компонент и настрой».
            //
            // Голый прямоугольник — это ещё не элемент: чтобы получить из него
            // кнопку, надо добавить подложку, надпись и реакцию на мышь. Меню
            // отдаёт то, что человек и хотел получить, сразу собранным; дальше
            // правится всё, вплоть до состава частей.
            if (ImGui::BeginMenu(T("Create UI"))) {
                struct Preset { const char* Name; const char* Label; };
                static const Preset kPresets[] = {
                    {"Panel", T("Panel")},   {"Button", T("Button")}, {"Label", T("Label")},
                    {"Image", T("Image")}, {"Bar", T("Bar")},    {"Checkbox", T("Checkbox")},
                    {"Slider", T("Slider")}, {"Input", T("Input")},
                };
                for (const Preset& p : kPresets) {
                    if (ImGui::MenuItem(p.Label)) {
                        PushUndoSnapshot();
                        SetSelectedId(CreateUIEntity(p.Name).Id());
                        // И сразу открывается редактор интерфейса: элемент,
                        // которого не видно после создания, выглядит как
                        // «кнопка не сработала».
                        m_showUIEditor = true;
                        m_uiEditor.RequestFocus();
                    }
                }

                // Готовые ЭКРАНЫ, а не отдельные элементы.
                //
                // Заготовка отвечает на вопрос «что такое кнопка»; оставшийся —
                // «как из кнопок собирают меню» — до сих пор оставался без
                // ответа, и каждый отвечал на него сам. Демо ставится в сцену и
                // разбирается в инспекторе: холст, раскладка, группа и имена
                // действий видны на работающем экране (см. sage/ui/UIDemos.h).
                ImGui::Separator();
                if (ImGui::BeginMenu(T("Demo screens"))) {
                    struct Demo { const char* Key; const char* Label; };
                    static const Demo kDemos[] = {
                        {"menu", T("Main menu")},
                        {"hud", T("HUD")},
                        {"settings", T("Settings")},
                    };
                    for (const Demo& d : kDemos) {
                        if (ImGui::MenuItem(d.Label)) {
                            PushUndoSnapshot();
                            const int id = sage::ui::BuildDemo(*m_scene, d.Key);
                            if (id >= 0) SetSelectedId(id);
                            m_showUIEditor = true;
                            m_uiEditor.RequestFocus();
                            m_sceneDirty = true;
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(T("Create Camera"))) {
                PushUndoSnapshot();
                GameObject camObj = m_scene->CreateObject("Camera");
                m_scene->Registry().emplace<CameraComponent>(camObj.Entity());
                SetSelectedId(camObj.Id());
            }
            // Свет — подменю по типам. Одного пункта «Create Light» было мало:
            // он всегда создавал точечный, а прожектор и солнце приходилось
            // делать через инспектор, догадавшись, что тип там переключается.
            if (ImGui::BeginMenu(T("Create Light"))) {
                struct LightPreset {
                    const char* item;
                    const char* name;
                    LightComponent::Type kind;
                    glm::vec3 position;
                    glm::vec3 rotation;
                };
                const LightPreset presets[] = {
                    {T("Point"), "Light", LightComponent::Type::Point,
                     {0.0f, 2.5f, 0.0f}, {0.0f, 0.0f, 0.0f}},
                    // Прожектор смотрит вниз: поворот -90° по X направляет
                    // «вперёд» (-Z) в -Y. Созданный «в никуда», он выглядел бы
                    // как свет, который не работает.
                    {T("Spot"), "Spotlight", LightComponent::Type::Spot,
                     {0.0f, 5.0f, 0.0f}, {-90.0f, 0.0f, 0.0f}},
                    {T("Directional (sun)"), "Sun", LightComponent::Type::Directional,
                     {0.0f, 10.0f, 0.0f}, {-55.0f, -25.0f, 0.0f}},
                };
                for (const LightPreset& p : presets) {
                    if (!ImGui::MenuItem(p.item)) continue;
                    PushUndoSnapshot();
                    GameObject lightObj = m_scene->CreateObject(p.name);
                    lightObj.GetTransform().Position = p.position;
                    lightObj.GetTransform().Rotation = p.rotation;
                    LightComponent lc;
                    lc.Kind = p.kind;
                    if (p.kind == LightComponent::Type::Directional) {
                        lc.Color = {1.0f, 0.95f, 0.85f};
                        lc.Intensity = 1.0f;
                    } else if (p.kind == LightComponent::Type::Spot) {
                        lc.Intensity = 4.0f;
                    }
                    m_scene->Registry().emplace<LightComponent>(lightObj.Entity(), lc);
                    SetSelectedId(lightObj.Id());
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            // Физический куб: меш + динамическое тело + бокс-коллайдер — падает
            // под гравитацией сразу в Play (быстрый способ проверить физику).
            if (ImGui::MenuItem(T("Create Physics Cube"))) {
                PushUndoSnapshot();
                GameObject box = CreatePrimitiveEntity("Physics Cube", MeshRef::Type::Cube);
                box.GetTransform().Position = {0.0f, 4.0f, 0.0f};
                m_scene->Registry().emplace<RigidBodyComponent>(box.Entity());
                m_scene->Registry().emplace<ColliderComponent>(box.Entity());
                SetSelectedId(box.Id());
            }
            // Скелетно-анимированная модель: без пути — процедурный демо-щупалец
            // с клипом «Wave» (сразу проигрывается в вьюпорте).
            if (ImGui::MenuItem(T("Create Animated Model"))) {
                PushUndoSnapshot();
                GameObject anim = m_scene->CreateObject("Animated Model");
                anim.GetTransform().Position = {0.0f, 0.0f, 0.0f};
                m_scene->Registry().emplace<AnimatedModelComponent>(anim.Entity());
                SetSelectedId(anim.Id());
            }
            // Эмиттер частиц: по умолчанию пресет «Fire» в точке над началом.
            if (ImGui::MenuItem(T("Create Particle Emitter"))) {
                PushUndoSnapshot();
                GameObject fx = m_scene->CreateObject("Particle Emitter");
                fx.GetTransform().Position = {0.0f, 0.5f, 0.0f};
                ParticleEmitterComponent em;
                em.Config = ParticlePresets::Registry()[0].Make(); // Fire
                em.Preset = 0;
                m_scene->Registry().emplace<ParticleEmitterComponent>(fx.Entity(), em);
                SetSelectedId(fx.Id());
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Window"))) {
            // Сброс раскладки возвращает и сами панели: закрытая вкладка иначе
            // не восстанавливалась «сбросом», хотя именно этого от него ждут.
            if (ImGui::MenuItem(T("Reset Layout"))) {
                ShowAllPanels();
                m_rebuildDockLayout = true;
            }
            ImGui::MenuItem(T("Show Grid"), nullptr, &m_showGrid);
            ImGui::Separator();
            // Каждая панель — переключатель. Это единственный путь назад после
            // крестика на вкладке, поэтому здесь перечислены ВСЕ панели, а не
            // только служебные.
            //
            // Флаги берутся через PanelVisible(EditorPanel) — тем же путём, что
            // и у кнопок верхней панели. Два списка полей рядом однажды
            // разъедутся: кнопка будет открывать одно окно, а галка меню —
            // отмечать другое.
            ImGui::MenuItem(T("Hierarchy"), nullptr, &PanelVisible(EditorPanel::Hierarchy));
            ImGui::MenuItem(T("Inspector"), nullptr, &PanelVisible(EditorPanel::Inspector));
            ImGui::MenuItem(T("Viewport"), nullptr, &PanelVisible(EditorPanel::Viewport));
            ImGui::MenuItem(T("Game"), nullptr, &PanelVisible(EditorPanel::Game));
            ImGui::MenuItem(T("Assets"), nullptr, &PanelVisible(EditorPanel::Assets));
            ImGui::MenuItem(T("Console"), nullptr, &PanelVisible(EditorPanel::Console));
            // «Освещение» стало «Средой»: в окне остались небо, воздух и
            // окружающий свет, а сами источники света — на объектах сцены.
            ImGui::MenuItem(T("Environment"), nullptr, &PanelVisible(EditorPanel::Environment));
            ImGui::MenuItem(T("Interface"), nullptr, &PanelVisible(EditorPanel::UIEditor));
            ImGui::MenuItem(T("Code"), nullptr, &PanelVisible(EditorPanel::Code));
            ImGui::MenuItem(T("Profiler"), nullptr, &PanelVisible(EditorPanel::Profiler));
            ImGui::MenuItem(T("Icon sheet"), nullptr, &m_showIconSheet);
            ImGui::Separator();
            ImGui::MenuItem(T("Game Settings..."), nullptr, &PanelVisible(EditorPanel::Settings));
            // Шаблоны — рядом с настройками игры и языком, потому что это
            // настройка ОКРУЖЕНИЯ, а не текущей сцены: что установлено у меня
            // на машине и откуда это брать.
            ImGui::MenuItem(T("Project templates..."), nullptr, &m_showTemplates);

            // Язык интерфейса. Здесь, а не в окне Settings: то окно правит
            // настройки ИГРЫ и сохраняется в проект, а язык — настройка
            // редактора и живёт в профиле пользователя. Смешать их значило бы
            // получить проект, навязывающий язык всем, кто его откроет.
            // Оформление — рядом с языком и по той же причине: это настройка
            // РЕДАКТОРА, она живёт в профиле человека, а не в проекте. Проект,
            // навязывающий свою тему каждому, кто его открыл, — не то, чего
            // ждут от проекта.
            if (ImGui::BeginMenu(T("Appearance"))) {
                for (const EditorTheme::Theme& theme : EditorTheme::Themes()) {
                    const bool active = theme.Id == EditorTheme::CurrentId();
                    // Название темы не переводится — как и название языка: по
                    // нему тему находят в файле настроек и в themes/*.json.
                    if (ImGui::MenuItem(theme.Name.c_str(), nullptr, active) && !active) {
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
                if (ImGui::MenuItem(T("Reset scale"), nullptr, false, scale != 1.0f))
                    EditorTheme::SetUiScale(1.0f);
                ImGui::Separator();
                // Выгрузка темы файлом — вот ради чего вся система: взять
                // текущую за основу, поправить цвета в текстовом редакторе и
                // положить обратно в themes/. Пересборка не нужна.
                if (ImGui::MenuItem(T("Export theme to themes/..."))) {
                    const std::string dir = EditorTheme::ThemesDir();
                    const std::string path = dir + "/" + EditorTheme::Current().Id + "-copy.json";
                    if (EditorTheme::ExportTheme(EditorTheme::Current(), path))
                        SetStatusMessage(std::string(T("Theme saved:")) + " " + path);
                    else
                        SetStatusMessage(T("Could not write the theme file"));
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(T("Language"))) {
                for (const sage::editor::LanguageInfo& lang : sage::editor::AvailableLanguages()) {
                    const bool active = lang.Code == sage::editor::CurrentLanguageCode();
                    // Название языка НЕ переводится: человек, случайно
                    // переключивший интерфейс на незнакомый, должен найти
                    // дорогу назад по слову «English», а не по переводу.
                    if (ImGui::MenuItem(lang.Name.c_str(), nullptr, active) && !active) {
                        sage::editor::SetLanguage(lang.Code);
                        SetStatusMessage(T("Interface language changed"));
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Help"))) {
            ImGui::MenuItem(T("About SAGE..."), nullptr, &m_showAbout);
            ImGui::EndMenu();
        }

        // --- СПРАВА: поиск команд, тема, проект --------------------------
        //
        // Собирается справа налево и всё — по токенам, поэтому блок не наезжает
        // на пункты меню при любой ширине окна: если места мало, первым уходит
        // поиск, потом имя проекта.
        {
            const Sage::UI::Style& ui = Sage::UI::Get();
            const std::string project = std::string(T("Project:")) + " " + m_project.Name();
            const float projectW = ImGui::CalcTextSize(project.c_str()).x + ui.SpacingMD;
            const float themesW = ui.ControlHeight * 2.0f + ui.SpacingSM;
            const float searchW = ui.ControlHeight * 9.0f;
            const float total = ImGui::GetWindowWidth();
            // Порог: строка меню занимает левую часть, и лезть на неё нельзя.
            const float menuEnd = ImGui::GetCursorPosX() + ui.SpacingLG;

            float x = total - ui.SpacingMD - projectW;
            const bool showProject = x > menuEnd;
            if (showProject) {
                ImGui::SameLine(x);
                ImGui::TextDisabled("%s", project.c_str());
            }

            x -= themesW;
            if (x > menuEnd) {
                ImGui::SameLine(x);
                // Тема — двумя кнопками, а не списком: переключений всего два, и
                // список ради двух пунктов — лишний щелчок каждый раз.
                const bool dark = EditorTheme::Current().Dark;
                if (Sage::UI::IconButton("sun", T("Light theme"), !dark))
                    EditorTheme::SetTheme("modern-light");
                ImGui::SameLine(0.0f, ui.SpacingXS);
                if (Sage::UI::IconButton("drop", T("Dark theme"), dark))
                    EditorTheme::SetTheme("modern-dark");
            }

            x -= searchW + ui.SpacingLG;
            if (x > menuEnd) {
                ImGui::SameLine(x);
                // Поле поиска ОТКРЫВАЕТ ПАЛИТРУ, а не ищет само: искать в
                // редакторе есть где (ассеты, иерархия), и второе поле с другим
                // поведением здесь путало бы. Щелчок = Ctrl+K.
                ImGui::SetNextItemWidth(searchW);
                ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::Input));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      EditorTheme::Color(EditorTheme::Role::Elevated));
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      EditorTheme::Color(EditorTheme::Role::TextFaint));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
                char label[96];
                std::snprintf(label, sizeof(label), "      %s  (Ctrl+K)", T("Search"));
                if (ImGui::Button(label, ImVec2(searchW, ui.ControlHeight))) m_palette.Open();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                const ImVec2 p0 = ImGui::GetItemRectMin();
                EditorIcons::Overlay(p0.x + ui.SpacingSM,
                                     p0.y + (ui.ControlHeight - ui.IconSize) * 0.5f, ui.IconSize,
                                     "search",
                                     glm::vec3(EditorTheme::Color(EditorTheme::Role::TextFaint).x,
                                               EditorTheme::Color(EditorTheme::Role::TextFaint).y,
                                               EditorTheme::Color(EditorTheme::Role::TextFaint).z));
            }
        }

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
    m_settingsPanel.Draw(*this, m_showSettings);
    m_templatesPanel.Draw(*this, m_showTemplates);
    m_profiler.Draw(&m_showProfiler);
    if (m_showIconSheet) EditorIcons::DrawSheet(&m_showIconSheet);
    m_confirm.Draw();
    DrawAboutWindow();

    ImGui::End();

    // Все панели закрыты — на месте редактора пустой прямоугольник. Молчать
    // здесь нельзя: человек видит серое поле и не знает, что перед ним
    // результат его же крестиков, а не поломка. Подсказка рисуется ПОСЛЕ
    // окна-хоста и отдельным окном: внутри хоста её накрывает фон пустого
    // док-узла, который ImGui кладёт в фоновый канал списка отрисовки.
    if (!AnyPanelVisible()) DrawEmptyDockHint(dockMin.x, dockMin.y, dockMax.x, dockMax.y);

    // Глобальные хоткеи (когда не печатаем в поле ввода).
    ImGuiIO& io = ImGui::GetIO();
    // Палитра команд — ДО проверки на ввод текста: Ctrl+K обязан открывать её
    // и из поля поиска ассетов, иначе «горячая клавиша, которая иногда не
    // работает» хуже отсутствующей.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) m_palette.Open(m_paletteQuery);
    m_palette.Draw(m_commands);

    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) DeleteSelected();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) DuplicateSelected();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && !m_scenePath.empty()) SaveSceneToFile(m_scenePath);
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
        if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
            (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))) Redo();
    }
}

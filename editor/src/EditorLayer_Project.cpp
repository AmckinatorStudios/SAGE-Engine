// ---------------------------------------------------------------------------
// EditorLayer — проект, сцена и сборка игры.
//
// Здесь всё, что отвечает на вопрос «с чем мы работаем»: создать сцену,
// открыть и сохранить её, завести или открыть проект, собрать из него игру,
// показать спасённое после падения. Одна связная область: у неё общий предмет
// (файлы проекта на диске) и общий способ ошибаться — путь, которого нет, или
// файл, который не читается.
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

constexpr float kStatusBarHeight = 26.0f;

} // namespace


// ============================================================================
//  Сцена / проект
// ============================================================================

void EditorLayer::NewScene(ProjectTemplateKind content) {
    if (InPlayMode()) StopPlay(); // нельзя подменять сцену под работающими скриптами
    m_undoStack.clear();
    m_redoStack.clear();
    m_scene = std::make_unique<Scene>("Untitled");
    SetSelectedId(-1);
    m_scenePath.clear();
    m_sceneDirty = false;

    if (content == ProjectTemplateKind::Demo) {
        // Скайбокс включён по умолчанию — сцена сразу с атмосферным фоном.
        m_scene->Lighting.Skybox.Enabled = true;

        struct Def { const char* name; glm::vec3 pos; glm::vec3 color; glm::vec3 scale; };
        Def defs[] = {
            {"Ground",     {0.0f, -0.75f, 0.0f}, {0.30f, 0.32f, 0.36f}, {6.0f, 0.3f, 6.0f}},
            {"Red Cube",   {-1.6f, 0.3f, 0.0f},  {0.85f, 0.30f, 0.30f}, {1.0f, 1.0f, 1.0f}},
            {"Green Cube", {0.0f, 0.3f, 0.0f},   {0.35f, 0.75f, 0.40f}, {1.0f, 1.0f, 1.0f}},
            {"Blue Cube",  {1.6f, 0.3f, 0.0f},   {0.35f, 0.55f, 0.90f}, {1.0f, 1.0f, 1.0f}},
            {"Tower",      {0.0f, 1.6f, -1.8f},  {0.90f, 0.80f, 0.35f}, {0.6f, 2.4f, 0.6f}},
        };
        for (const Def& d : defs) {
            GameObject obj = CreateCubeEntity(d.name);
            obj.GetTransform().Position = d.pos;
            obj.GetTransform().Scale = d.scale;
            obj.Renderer().Color = d.color;
        }

        // Криволинейные примитивы — витрина форм И проверка аутлайна выделения:
        // кайма строится из СИЛУЭТА реального меша, поэтому одинаково точна для
        // сферы/цилиндра/конуса, а не только для боксов.
        struct Prim { const char* name; MeshRef::Type type; glm::vec3 pos; glm::vec3 color; };
        Prim prims[] = {
            {"Sphere",   MeshRef::Type::Sphere,   {-3.0f, 0.5f, 1.8f}, {0.85f, 0.55f, 0.25f}},
            {"Cylinder", MeshRef::Type::Cylinder, {-1.5f, 0.5f, 2.2f}, {0.55f, 0.35f, 0.80f}},
            {"Cone",     MeshRef::Type::Cone,     {1.5f,  0.5f, 2.2f}, {0.30f, 0.70f, 0.70f}},
        };
        for (const Prim& p : prims) {
            GameObject obj = CreatePrimitiveEntity(p.name, p.type);
            obj.GetTransform().Position = p.pos;
            obj.Renderer().Color = p.color;
        }

        // Игровая камера сцены — панель Game сразу показывает картинку. НАРОЧНО
        // поставлена НЕ как редакторская орбитальная камера ({6.5,5,6.5}, взгляд
        // сверху): низкий, почти фронтальный «кинематографичный» ракурс с уровня
        // сцены — так сразу видно, что панель Game показывает СВОЮ, игровую
        // камеру, а не вид вьюпорта. Сущность без меша (не рисуется в мире).
        GameObject camObj = m_scene->CreateObject("Main Camera");
        camObj.GetTransform().Position = {0.0f, 1.5f, 6.5f};
        camObj.GetTransform().Rotation = {-6.0f, 0.0f, 0.0f}; // чуть вниз, вдоль -Z
        m_scene->Registry().emplace<CameraComponent>(camObj.Entity());

        // Солнце — такая же сущность, как всё остальное. Раньше его роль играли
        // три поля в настройках сцены; теперь его видно в иерархии, можно
        // повернуть гизмо и увидеть, как поехали тени.
        GameObject sun = m_scene->CreateObject("Sun");
        sun.GetTransform().Position = {0.0f, 10.0f, 0.0f};
        sun.GetTransform().Rotation =
            sage::ecs::EulerFromForward(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)));
        LightComponent sunLc;
        sunLc.Kind = LightComponent::Type::Directional;
        sunLc.Color = {1.0f, 0.95f, 0.85f};
        sunLc.Intensity = 1.0f;
        m_scene->Registry().emplace<LightComponent>(sun.Entity(), sunLc);

        // Тёплая лампа — демонстрация точечного света-сущности (LightComponent).
        GameObject lamp = m_scene->CreateObject("Lamp");
        lamp.GetTransform().Position = {2.4f, 1.6f, 1.8f};
        m_scene->Registry().emplace<LightComponent>(lamp.Entity());

        // Прожектор сверху — демонстрация конусного света (Spot): смотрит вниз
        // (поворот -90° по X направляет «вперёд» -Z в -Y), кладёт круг света
        // на кубы и пол.
        GameObject spot = m_scene->CreateObject("Spotlight");
        spot.GetTransform().Position = {0.0f, 5.0f, 0.0f};
        spot.GetTransform().Rotation = {-90.0f, 0.0f, 0.0f};
        LightComponent spotLc;
        spotLc.Kind = LightComponent::Type::Spot;
        spotLc.Color = {0.55f, 0.7f, 1.0f};
        spotLc.Intensity = 4.0f;
        spotLc.Range = 12.0f;
        spotLc.InnerConeDeg = 18.0f;
        spotLc.OuterConeDeg = 30.0f;
        m_scene->Registry().emplace<LightComponent>(spot.Entity(), spotLc);

        // Демо-худ: панель со скруглением и рамкой + полоса здоровья ребёнком.
        // Показывает систему интерфейса сразу в панели Game и служит стартовой
        // точкой для своего интерфейса (правится в Inspector).
        //
        // Собирается ИЗ ЧАСТЕЙ — так же, как это делает человек в инспекторе:
        // панель это прямоугольник + подложка + надпись, полоса — прямоугольник
        // + подложка + шкала.
        entt::registry& reg = m_scene->Registry();
        GameObject hud = m_scene->CreateObject("HUD Panel");
        sage::ui::Transform hudXf;
        hudXf.Anchor = UIAnchor::TopLeft;
        hudXf.Offset = {16.0f, 16.0f};
        hudXf.Size = {230.0f, 64.0f};
        reg.emplace<sage::ui::Transform>(hud.Entity(), hudXf);
        sage::ui::Fill hudFill;
        hudFill.Rounding = 12.0f;
        hudFill.BorderThickness = 2.0f;
        reg.emplace<sage::ui::Fill>(hud.Entity(), hudFill);
        sage::ui::Label hudLabel;
        hudLabel.Text = "SAGE UI";
        hudLabel.Horizontal = sage::ui::Label::Align::Start;
        reg.emplace<sage::ui::Label>(hud.Entity(), hudLabel);

        GameObject hp = m_scene->CreateObject("HP Bar");
        sage::ui::Transform hpXf;
        hpXf.Anchor = UIAnchor::BottomLeft;   // внутри панели-родителя
        hpXf.Offset = {12.0f, 8.0f};
        hpXf.Size = {206.0f, 18.0f};
        reg.emplace<sage::ui::Transform>(hp.Entity(), hpXf);
        sage::ui::Fill hpFill;
        hpFill.Rounding = 8.0f;
        hpFill.Color = {0.0f, 0.0f, 0.0f, 0.55f};
        reg.emplace<sage::ui::Fill>(hp.Entity(), hpFill);
        sage::ui::Bar hpBar;
        hpBar.Value = 0.72f;
        hpBar.FillColor = {0.85f, 0.30f, 0.30f, 1.0f};
        hpBar.Smoothing = 3.0f;
        reg.emplace<sage::ui::Bar>(hp.Entity(), hpBar);
        m_scene->SetParent(hp.Entity(), hud.Entity());

    } else if (content == ProjectTemplateKind::UIStarter) {
        // Стартер интерфейса: сцены как таковой нет, зато есть камера, свет и
        // два готовых экрана. С этого начинают те, кому нужна не витрина
        // движка, а меню и худ, — и раньше им приходилось сначала удалить
        // девять чужих объектов, а потом собрать экраны с нуля.
        m_scene->Lighting.Skybox.Enabled = true;

        GameObject ground = CreatePrimitiveEntity("Ground", MeshRef::Type::Plane);
        ground.GetTransform().Scale = {12.0f, 1.0f, 12.0f};
        ground.Renderer().Color = {0.30f, 0.32f, 0.36f};

        GameObject camObj = m_scene->CreateObject("Main Camera");
        camObj.GetTransform().Position = {0.0f, 1.6f, 6.0f};
        camObj.GetTransform().Rotation = {-8.0f, 0.0f, 0.0f};
        m_scene->Registry().emplace<CameraComponent>(camObj.Entity());

        GameObject sun = m_scene->CreateObject("Sun");
        sun.GetTransform().Rotation =
            sage::ecs::EulerFromForward(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)));
        LightComponent sunLc;
        sunLc.Kind = LightComponent::Type::Directional;
        sunLc.Color = {1.0f, 0.95f, 0.85f};
        m_scene->Registry().emplace<LightComponent>(sun.Entity(), sunLc);

        // Те же самые демо-экраны, что и в меню «Create UI»: одна реализация,
        // а не «похожий интерфейс, собранный отдельно для шаблона».
        sage::ui::BuildDemo(*m_scene, "hud");
        sage::ui::BuildDemo(*m_scene, "menu");
    }

    if (content == ProjectTemplateKind::Demo) {
        // Что-то выбрано сразу — гизмо видно, Inspector не пустой. Выбираем
        // криволинейный примитив: сразу демонстрирует аутлайн на изогнутом
        // силуэте (кайма строится из силуэта меша — точна для любой формы).
        GameObject sel = m_scene->FindByName("Cone");
        if (!sel.Valid()) sel = m_scene->FindByName("Green Cube");
        if (sel.Valid()) SetSelectedId(sel.Id());
    }
    UpdateWindowTitle();
}

bool EditorLayer::LoadSceneFromFile(const fs::path& path) {
    if (InPlayMode()) StopPlay(); // см. NewScene
    try {
        m_scene = SceneSerializer::Load(path.string());
        m_undoStack.clear();
        m_redoStack.clear();
        SetSelectedId(-1);
        m_scenePath = path;
        m_sceneDirty = false;
        LOG_INFO("Editor") << "Scene loaded: " << path.string();
        UpdateWindowTitle();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Scene load failed: " << e.what();
        return false;
    }
}

// Предложение восстановить сцену после падения или из автосохранения.
//
// Модалка, а не тихая загрузка: восстановленная сцена может быть НЕ той, над
// которой человек работал последней (например, он с тех пор открыл другой
// проект), и решать это должен он, а не редактор.
void EditorLayer::DrawRecoveryPrompt() {
    if (!m_recoveryPrompt) return;
    ImGui::OpenPopup(T("Restore scene?"));
    if (ImGui::BeginPopupModal(T("Restore scene?" "###Restore scene?"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T("The previous session seems to have crashed"));
        ImGui::Spacing();
        ImGui::Text(T("Found file: %s"), m_recoveryFile.c_str());
        ImGui::TextDisabled("%s", T("It did not overwrite your scene — this is a separate copy."));
        ImGui::Spacing();
        if (ImGui::Button(T("Open the copy"), ImVec2(150, 0))) {
            if (LoadSceneFromFile(m_recoveryFile)) {
                // Путь сцены НЕ ставим: иначе первое же Ctrl+S записало бы
                // восстановленное поверх файла восстановления, а не сцены.
                m_scenePath.clear();
                m_sceneDirty = true;
                UpdateWindowTitle();
            }
            m_recoveryPrompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Keep the file"), ImVec2(150, 0))) {
            m_recoveryPrompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Delete"), ImVec2(110, 0))) {
            std::error_code ec;
            fs::remove(m_recoveryFile, ec);
            m_recoveryPrompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool EditorLayer::SaveSceneToFile(const fs::path& path) {
    try {
        std::error_code ec;
        if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
        SceneSerializer::Save(*m_scene, path.string());
        m_scenePath = path;
        m_sceneDirty = false;
        LOG_INFO("Editor") << "Scene saved: " << path.string();
        UpdateWindowTitle();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Scene save failed: " << e.what();
        return false;
    }
}

// Заметка шаблона — В КОНСОЛЬ, а не только в диалог.
//
// Из стартового окна проект создают без диалога, и пояснение там показать
// негде; а именно там его и не хватает: витрина строит мир скриптами, и
// созданный проект выглядит пустым. Строка в консоли — то место, куда человек
// смотрит, когда что-то «не работает», и она обязана быть там раньше вопроса.
void EditorLayer::AnnounceTemplateNote(const ProjectTemplate& tpl) {
    m_templateNote.clear();
    if (tpl.Note[0] == '\0') return;
    m_templateNote = sage::editor::T(tpl.Note);
    // Через T(): заметка — строка ИНТЕРФЕЙСА, и в консоли она обязана быть на
    // том же языке, что и всё остальное.
    LOG_INFO("Editor") << sage::editor::T(tpl.Note);
}


// --- Съёмка обложек шаблонов -------------------------------------------------
//
// По кадру на шаблон: создать проект, дать сцене устояться, снять игровой кадр.
// Машина состояний, а не цикл, потому что снимок берётся ПОСЛЕ RenderGame —
// внутри кадра, а не между вызовами.
//
// У шаблона-копии (витрина) мир собирают скрипты, и в остановленном редакторе
// сцена почти пуста. Обложка обязана показывать проект таким, каким он будет,
// поэтому для него включается Play и делается несколько шагов скриптов — иначе
// снимок честно покажет пустоту и обманет ровно так же, как рисунок.
void EditorLayer::TickTemplateShots() {
    if (m_coverShotDir.empty()) return;
    const std::vector<ProjectTemplate>& all = ProjectTemplates();

    // Ждём, пока снимется текущий: съёмка происходит в конце кадра.
    if (!m_coverShotPath.empty()) return;

    if (m_coverShotIndex >= 0 && !m_coverShotDone && m_coverShotWait > 0) {
        // Кадры «на устояться»: первый кадр после загрузки сцены ещё не имеет
        // ни теней, ни отражений, ни собранного скриптами мира.
        --m_coverShotWait;
        if (InPlayMode() && m_playScripts) m_playScripts->UpdateAll(1.0f / 60.0f);
        if (m_coverShotWait == 0) {
            m_coverShotPath =
                (std::filesystem::path(m_coverShotDir) / (all[(size_t)m_coverShotIndex].Id + ".png"))
                    .string();
        }
        return;
    }

    // Предыдущий снят — переходим к следующему шаблону.
    const int next = m_coverShotIndex + 1;
    if (next >= (int)all.size()) {
        LOG_INFO("Editor") << "Обложки шаблонов сняты: " << all.size();
        m_coverShotDir.clear();
        sage::Application::Get().Close();
        return;
    }
    m_coverShotIndex = next;
    m_coverShotDone = false;

    const ProjectTemplate& tpl = all[(size_t)next];
    // Один шаблон вместо всех: SAGE_EDITOR_TEMPLATE рядом с папкой снимков.
    // Нужно и для пересъёмки одной обложки, и для разбора — свежий процесс на
    // один шаблон исключает влияние предыдущей сцены.
    if (const char* only = std::getenv("SAGE_EDITOR_TEMPLATE")) {
        if (tpl.Id != only) { m_coverShotWait = 0; return; }
    }
    if (!ProjectTemplateAvailable(tpl)) {
        LOG_WARN("Editor") << "Обложка пропущена: шаблон " << tpl.Id << " не установлен";
        m_coverShotWait = 0;
        return;
    }
    if (InPlayMode()) StopPlay();
    std::error_code ec;
    const std::string dir = "sage_cover_" + tpl.Id;
    std::filesystem::remove_all(dir, ec);
    std::string err;
    if (!CreateProject(".", dir, tpl.Id, err)) {
        LOG_ERROR("Editor") << "Обложка: проект по шаблону " << tpl.Id << " не создался: " << err;
        m_coverShotWait = 0;
        return;
    }
    // Мир, который строят скрипты, обязан быть построен ДО снимка.
    if (tpl.Kind == ProjectTemplateKind::Copy) StartPlay();
    m_coverShotWait = 12;
}

bool EditorLayer::CreateProject(const std::string& dir, const std::string& name,
                               const std::string& templateId, std::string& err) {
    // Шаблон разбирается ДО создания папок: получить проект и узнать, что имя
    // шаблона не то, — худший из порядков.
    const ProjectTemplate* tpl = FindProjectTemplate(templateId);
    if (!tpl) {
        std::string known;
        for (const ProjectTemplate& t : ProjectTemplates()) {
            if (!known.empty()) known += ", ";
            known += t.Id;
        }
        err = "Unknown project template '" + templateId + "'; known: " + known;
        return false;
    }
    // Готовый проект обязан быть на диске ДО создания папок: иначе человек
    // получил бы пустой проект с именем «Витрина» и никакого объяснения.
    if (tpl->Kind == ProjectTemplateKind::Copy && !ProjectTemplateAvailable(*tpl)) {
        err = "Template '" + tpl->Id + "' is not installed next to the editor (" +
              (ProjectTemplatesRoot() / tpl->SourceDir).string() + ")";
        return false;
    }
    if (!m_project.CreateNew(dir, name, err)) return false;
    const ProjectTemplateKind kind = tpl->Kind;
    m_assetsCwd = m_project.Dir();
    m_recent.Add(m_project.Dir().string());

    if (kind == ProjectTemplateKind::Copy) {
        // Копия ГОТОВОГО проекта: сцены, скрипты, модели и настройки — как
        // есть. Имя проекта при этом остаётся тем, которое выбрал человек:
        // копируется содержимое, а не чужая вывеска.
        const fs::path src = ProjectTemplatesRoot() / tpl->SourceDir;
        std::error_code ec;
        for (const fs::directory_entry& e : fs::directory_iterator(src, ec)) {
            // Файл проекта пропускаем — он уже создан с нужным именем.
            if (e.path().filename() == "project.sageproj") continue;
            fs::copy(e.path(), m_project.Dir() / e.path().filename(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                err = "Failed to copy template '" + tpl->Id + "': " + ec.message();
                return false;
            }
        }
        m_project.Adopt();          // папки на месте, база ассетов знает корень
        m_settings = sage::EngineConfig{};
        m_settings.LoadFile((m_project.Dir() / "sage.cfg").string());
        m_settings.ApplyEnvOverrides();
        ApplyEngineSettings();
        // Открываем первую сцену копии — иначе человек видит демо-сцену
        // редактора вместо проекта, который только что попросил.
        std::vector<fs::path> scenes;
        for (const fs::directory_entry& e : fs::directory_iterator(m_project.ScenesDir(), ec)) {
            if (e.path().extension() == ".sage") scenes.push_back(e.path());
        }
        std::sort(scenes.begin(), scenes.end());
        if (scenes.empty()) {
            err = "Template '" + tpl->Id + "' has no scenes";
            return false;
        }
        if (!LoadSceneFromFile(scenes.front())) {
            err = "Failed to load the template scene: " + scenes.front().string();
            return false;
        }
        LOG_INFO("Editor") << "Project created from template '" << tpl->Id << "': "
                           << m_project.Dir().string();
        AnnounceTemplateNote(*tpl);
        UpdateWindowTitle();
        return true;
    }

    NewScene(kind);
    AnnounceTemplateNote(*tpl);
    UpdateWindowTitle();
    return true;
}

bool EditorLayer::OpenProject(const std::string& path, std::string& err) {
    if (!m_project.Open(path, err)) return false;
    m_assetsCwd = m_project.Dir();
    m_recent.Add(m_project.Dir().string());

    // Настройки проекта (sage.cfg) — в окно Settings; отсутствие файла не ошибка
    // (остаются значения по умолчанию).
    m_settings = sage::EngineConfig{};
    m_settings.LoadFile((m_project.Dir() / "sage.cfg").string());
    m_settings.ApplyEnvOverrides();   // SAGE_* поверх файла — как у рантайма
    ApplyEngineSettings();

    // Автозагрузка первой сцены проекта (по алфавиту) — открытый проект сразу
    // показывает свой контент, а не осиротевшую демо-сцену.
    std::error_code ec;
    std::vector<fs::path> scenes;
    for (const auto& entry : fs::directory_iterator(m_project.ScenesDir(), ec)) {
        if (entry.path().extension() == ".sage") scenes.push_back(entry.path());
    }
    std::sort(scenes.begin(), scenes.end());
    if (!scenes.empty()) LoadSceneFromFile(scenes.front());

    UpdateWindowTitle();
    return true;
}

// ============================================================================
//  Сборка игры: SagePlayer + рантайм-ассеты + project/ => запускаемая папка
// ============================================================================

bool EditorLayer::BuildGame(const fs::path& outputDir, std::string& err) {
    if (!m_project.Loaded()) {
        err = "No project open";
        return false;
    }

    // 1. Собранный SagePlayer: явный SAGE_PLAYER_PATH, иначе стандартные
    // места относительно редактора (../runtime в дереве сборки, рядом с exe).
#ifdef _WIN32
    const char* playerName = "SagePlayer.exe";
    std::string exeSuffix = ".exe";
#else
    const char* playerName = "SagePlayer";
    std::string exeSuffix;
#endif
    // Ищем ОТ СВОЕГО БИНАРНИКА, а не от текущей папки.
    //
    // Здесь стояли только относительные пути («./SagePlayer», «../runtime/
    // SagePlayer»), то есть поиск шёл от ТЕКУЩЕЙ ПАПКИ ПРОЦЕССА. В дереве
    // сборки это совпадало с папкой редактора и работало, а у человека,
    // запустившего установленный редактор ярлыком, текущей папкой оказывались
    // «Документы» — и сборка игры падала с «SagePlayer not found» при плеере,
    // лежащем в двух шагах, рядом с самим редактором. Текущая папка — это
    // откуда ЗАПУСКАЮТ, а не куда УСТАНОВЛЕНО (ровно та же ошибка, из-за
    // которой появился sage/core/Paths.h).
    const fs::path exeDir = sage::ExecutableDir();
    std::vector<fs::path> candidates;
    if (const fs::path p = sage::EnvPath("SAGE_PLAYER_PATH"); !p.empty())
        candidates.push_back(p);
    if (!exeDir.empty()) {
        candidates.push_back(exeDir / playerName);                        // установка: рядом с редактором
        candidates.push_back(exeDir / "runtime" / playerName);            // установка с подпапками
        candidates.push_back(exeDir.parent_path() / "runtime" / playerName); // дерево сборки
        candidates.push_back(exeDir / ".." / "runtime" / playerName);
    }
    candidates.push_back(fs::path("..") / "runtime" / playerName); // запуск из папки сборки
    candidates.push_back(fs::path(".") / playerName);

    std::error_code ec;
    fs::path player;
    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) {
            player = candidate;
            break;
        }
    }
    if (player.empty()) {
        // Сообщение называет ВСЕ просмотренные места. «Не найдено» без списка
        // не отличает «плеер не собран» от «редактор ищет не там», а починить
        // надо разное.
        std::string where;
        for (const fs::path& candidate : candidates) {
            where += "\n  " + fs::weakly_canonical(candidate, ec).string();
        }
        err = std::string(T("SagePlayer not found. Put it next to the editor, build the SagePlayer "
                            "target, or set SAGE_PLAYER_PATH. Looked in:")) +
              where;
        LOG_ERROR("Editor") << "Сборка игры: плеер не найден. Искали:" << where;
        return false;
    }
    LOG_INFO("Editor") << "Сборка игры: плеер " << fs::weakly_canonical(player, ec).string();

    // 2. Слепить папку игры: <out>/<Name>/{<Name>, assets/(рантайм), project/}.
    fs::path gameDir = outputDir / m_project.Name();
    fs::remove_all(gameDir, ec); // пересборка затирает прошлую (это артефакт, не данные)
    fs::create_directories(gameDir, ec);
    if (ec) {
        err = "Cannot create " + gameDir.string() + ": " + ec.message();
        return false;
    }

    fs::copy_file(player, gameDir / (m_project.Name() + exeSuffix),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "Player copy failed: " + ec.message();
        return false;
    }
    fs::copy(player.parent_path() / "assets", gameDir / "assets",
             fs::copy_options::recursive, ec);
    if (ec) {
        err = "Runtime assets copy failed: " + ec.message();
        return false;
    }
    // Проект едет в игру ПАКЕТОМ (game.sagepak), а не россыпью файлов.
    //
    // Копирование папки как есть означало три вещи сразу: медленный старт
    // (тысяча мелких файлов открывается дольше одного большого), игру, которую
    // открывают блокнотом (исходные .lua и .sage лежат рядом с exe), и лишний
    // размер (текстовые сцены и скрипты жмутся в разы).
    //
    // Файлы .meta и .sageimport в пакет не кладутся: это служебные данные
    // редактора (GUID'ы ассетов, параметры импорта), в игре по ним никто не
    // ходит, а место они занимают.
    {
        sage::assets::PackWriter pack;
        // project.sageproj в пакет НЕ кладётся, а копируется рядом с exe. Это
        // манифест игры: по нему плеер узнаёт её имя, и на него указывают,
        // когда запускают игру из другой папки или перетаскивают файл на
        // плеер. Спрятав его в пакет, мы отняли бы этот способ запуска — что и
        // случилось, и поймал это smoke-тест «игра запускается из ЛЮБОЙ папки».
        //
        // Копия одна, а не две: файла нет в пакете, поэтому правила «пакет
        // против россыпи» он не нарушает.
        const size_t packed =
            pack.AddDirectory(m_project.Dir(), {".meta", ".sageimport", "project.sageproj"});
        if (!pack.Save(gameDir / "game.sagepak")) {
            err = T("Could not write the game package");
            return false;
        }
        fs::copy_file(m_project.Dir() / "project.sageproj", gameDir / "project.sageproj",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            err = T("Could not copy the project file: ") + ec.message();
            return false;
        }
        LOG_INFO("Editor") << "Пакет игры: " << packed << " файлов";
    }

    // Настройки проекта — рядом с exe игры (sage.cfg), чтобы игрок мог править их
    // без залезания в project/. SagePlayer грузит и этот, и project/sage.cfg.
    std::error_code cfgEc;
    fs::path projCfg = m_project.Dir() / "sage.cfg";
    if (fs::exists(projCfg, cfgEc)) {
        fs::copy_file(projCfg, gameDir / "sage.cfg", fs::copy_options::overwrite_existing, cfgEc);
    }

    LOG_INFO("Editor") << "Game built: " << gameDir.string();
    return true;
}

// Заголовок OS-окна: "SAGE Editor — сцена[*] — проект". Обновляется только
// по факту изменения (не дёргаем GLFW каждый кадр).
void EditorLayer::UpdateWindowTitle() {
    std::string scene = m_scenePath.empty() ? m_scene->Name() : m_scenePath.filename().string();
    std::string title = "SAGE Editor — " + scene + (m_sceneDirty ? "*" : "");
    if (m_project.Loaded()) title += " — " + m_project.Name();
    if (title == m_windowTitle) return;
    m_windowTitle = title;
    glfwSetWindowTitle(sage::Application::Get().GetWindow().Handle(), title.c_str());
}

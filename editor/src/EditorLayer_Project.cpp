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
#include "CodeEditorApp.h"
#include "ProjectLauncher/ProjectDatabase.h"
#include "SceneCover.h"
#include "Thumbnails.h"
#include "sage/assets/Pack.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <cfloat>

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

// Копирование каталога В УЖЕ СУЩЕСТВУЮЩИЙ каталог, файл за файлом.
//
// ЗАЧЕМ, если есть fs::copy с recursive|overwrite_existing. Затем, что НА
// WINDOWS ОН ЭТОГО НЕ ДЕЛАЕТ: когда каталог-приёмник уже есть, реализация
// отдаёт EEXIST («File exists») и не копирует НИЧЕГО. На Linux тот же вызов
// молча сливает содержимое, поэтому беда не видна ни в CI, ни на машине
// разработчика — ровно тот случай, про который CLAUDE.md говорит «правка,
// проверенная на одной платформе, проверена наполовину».
//
// Стоило это создания проекта из готового шаблона: CreateNew уже завёл
// assets/ и scenes/, дальше шаблон копировался поверх — и на Windows человек
// получал отказ «Failed to copy template … File exists» и пустой проект.
void CopyInto(const fs::path& src, const fs::path& dst, std::error_code& ec) {
    if (!fs::is_directory(src, ec)) {
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        return;
    }
    fs::create_directories(dst, ec);
    if (ec) return;
    for (const fs::directory_entry& e : fs::directory_iterator(src, ec)) {
        if (ec) return;
        CopyInto(e.path(), dst / e.path().filename(), ec);
        if (ec) return;
    }
}

} // namespace


// ============================================================================
//  Сцена / проект
// ============================================================================

bool EditorLayer::OpenFileInSystemEditor(const fs::path& path, int line) {
    // Сначала ВЫБРАННЫЙ редактор (VS Code, CLion, Sublime…): он один умеет
    // встать на нужную строку, и он один точно установлен — в списке только то,
    // что нашлось на этой машине (см. CodeEditorApp.h).
    if (sage::editor::codeapp::Open(path, line)) return true;

    // Запасной путь — системная ассоциация. Той же функцией, которой лаунчер
    // показывает папку проекта: «открой это тем, чем открываешь обычно».
    const bool ok = Sage::Launcher::OpenWithSystem(path);
    if (!ok) LOG_WARN("Editor") << "Система не открыла файл: " << path.string();
    return ok;
}

// Материал шаблона: файл в assets/materials/<имя>.sagemat + ссылка на него.
// Почему файлом, а не полем Color — см. объявление в EditorLayer.h.
std::string EditorLayer::TemplateMaterial(const std::string& name, const glm::vec3& albedo,
                                          float metallic, float roughness) {
    if (!m_project.Loaded()) return {};
    std::error_code ec;
    const fs::path dir = m_project.AssetsDir() / "materials";
    fs::create_directories(dir, ec);
    if (ec) return {};

    Material mat;
    mat.Albedo = albedo;
    mat.Metallic = metallic;
    mat.Roughness = roughness;
    const fs::path path = dir / (name + ".sagemat");
    mat.SaveToFile(sage::PathToUtf8(path));
    if (!fs::exists(path, ec)) return {};

    // Ссылка ОТНОСИТЕЛЬНАЯ (assets/materials/...): абсолютный путь пережил бы
    // ровно до первого переноса проекта на другую машину.
    return m_project.AssetRef(path);
}

void EditorLayer::PaintWithMaterial(GameObject object, const std::string& name,
                                    const glm::vec3& albedo, float metallic, float roughness) {
    MeshRendererComponent& mr = object.Renderer();
    const std::string ref = TemplateMaterial(name, albedo, metallic, roughness);
    if (ref.empty()) {
        // Проекта нет — файл писать некуда, и цвет остаётся у объекта. Это не
        // «второй способ красить»: ровно для такого случая Color и оставлен —
        // объект БЕЗ материала (см. ecs/RenderComponents.h).
        mr.Color = albedo;
        return;
    }
    AssignMaterial(mr, ref, ResourceManager::Instance().GetMaterial(ref));
}

void EditorLayer::NewScene(ProjectTemplateKind content) {
    if (InPlayMode()) StopPlay(); // нельзя подменять сцену под работающими скриптами
    m_history.Clear();
    m_scene = std::make_unique<Scene>("Untitled");
    m_selection.SetPrimary(-1);
    m_scenePath.clear();
    m_sceneDirty = false;

    if (content == ProjectTemplateKind::Demo) {
        // Скайбокс включён по умолчанию — сцена сразу с атмосферным фоном.
        m_scene->Lighting.Skybox.Enabled = true;

        // ЦВЕТ ПРИХОДИТ ИЗ МАТЕРИАЛА, а не из поля объекта.
        //
        // Шаблон — это ещё и образец того, как устроен проект: по нему учатся,
        // открыв инспектор первого же куба. Пока кубы красились полем Color,
        // образец врал: цвет был, материала не было, и откуда взялся цвет,
        // ответить было нечем. Теперь у каждой формы свой .sagemat в
        // assets/materials — тот самый файл, который человек и правит дальше.
        //
        // Шероховатость у металла (Tower) ниже: витрина обязана показывать не
        // только разные цвета, но и разные ПОВЕРХНОСТИ — иначе непонятно,
        // зачем материалу что-то кроме цвета.
        struct Def {
            const char* name; glm::vec3 pos; glm::vec3 color; glm::vec3 scale;
            float metallic; float roughness;
        };
        Def defs[] = {
            {"Ground",     {0.0f, -0.75f, 0.0f}, {0.30f, 0.32f, 0.36f}, {6.0f, 0.3f, 6.0f}, 0.0f, 0.85f},
            {"Red Cube",   {-1.6f, 0.3f, 0.0f},  {0.85f, 0.30f, 0.30f}, {1.0f, 1.0f, 1.0f}, 0.0f, 0.55f},
            {"Green Cube", {0.0f, 0.3f, 0.0f},   {0.35f, 0.75f, 0.40f}, {1.0f, 1.0f, 1.0f}, 0.0f, 0.55f},
            {"Blue Cube",  {1.6f, 0.3f, 0.0f},   {0.35f, 0.55f, 0.90f}, {1.0f, 1.0f, 1.0f}, 0.0f, 0.55f},
            {"Tower",      {0.0f, 1.6f, -1.8f},  {0.90f, 0.80f, 0.35f}, {0.6f, 2.4f, 0.6f}, 1.0f, 0.25f},
        };
        for (const Def& d : defs) {
            GameObject obj = CreateCubeEntity(d.name);
            obj.GetTransform().Position = d.pos;
            obj.GetTransform().Scale = d.scale;
            PaintWithMaterial(obj, d.name, d.color, d.metallic, d.roughness);
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
            PaintWithMaterial(obj, p.name, p.color, 0.0f, 0.45f);
        }

        // СВЕТ, КАМЕРА И ЭЛЕМЕНТЫ ИНТЕРФЕЙСА СОЗДАЮТСЯ ПУСТЫМИ ОБЪЕКТАМИ.
        //
        // Scene::CreateObject вешает MeshRenderer (см. Scene.h) — и лампа в
        // шаблоне приезжала с компонентом «Меш»: в инспекторе секция с моделью,
        // цветом и тенями, которых у света нет вовсе. Шаблон — это ещё и
        // образец того, как устроен проект: по нему учатся, открыв инспектор
        // первого же объекта, и лишний компонент учит неправильному. Объект,
        // который ничего не рисует мешем, обязан заводиться через
        // CreateEmptyObject — ровно так же, как это делает каталог объектов.
        //
        // Игровая камера сцены — панель Game сразу показывает картинку. НАРОЧНО
        // поставлена НЕ как редакторская орбитальная камера ({6.5,5,6.5}, взгляд
        // сверху): низкий, почти фронтальный «кинематографичный» ракурс с уровня
        // сцены — так сразу видно, что панель Game показывает СВОЮ, игровую
        // камеру, а не вид вьюпорта. Сущность без меша (не рисуется в мире).
        GameObject camObj = m_scene->CreateEmptyObject("Main Camera");
        camObj.GetTransform().Position = {0.0f, 1.5f, 6.5f};
        camObj.GetTransform().Rotation = {-6.0f, 0.0f, 0.0f}; // чуть вниз, вдоль -Z
        m_scene->Registry().emplace<CameraComponent>(camObj.Entity());

        // Солнце — такая же сущность, как всё остальное. Раньше его роль играли
        // три поля в настройках сцены; теперь его видно в иерархии, можно
        // повернуть гизмо и увидеть, как поехали тени.
        GameObject sun = m_scene->CreateEmptyObject("Sun");
        sun.GetTransform().Position = {0.0f, 10.0f, 0.0f};
        sun.GetTransform().Rotation =
            sage::ecs::EulerFromForward(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)));
        LightComponent sunLc;
        sunLc.Kind = LightComponent::Type::Directional;
        sunLc.Color = {1.0f, 0.95f, 0.85f};
        sunLc.Intensity = 1.0f;
        m_scene->Registry().emplace<LightComponent>(sun.Entity(), sunLc);

        // Тёплая лампа — демонстрация точечного света-сущности (LightComponent).
        GameObject lamp = m_scene->CreateEmptyObject("Lamp");
        lamp.GetTransform().Position = {2.4f, 1.6f, 1.8f};
        m_scene->Registry().emplace<LightComponent>(lamp.Entity());

        // Прожектор сверху — демонстрация конусного света (Spot): смотрит вниз
        // (поворот -90° по X направляет «вперёд» -Z в -Y), кладёт круг света
        // на кубы и пол.
        GameObject spot = m_scene->CreateEmptyObject("Spotlight");
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
        // Собирается ИЗ ТИПОВ, а не из частей руками: панель это тип «Panel»,
        // полоса — тип «Bar», и ровно так их создаёт человек в окне
        // «Элементы». Демо, собранное в обход типов, давало бы элементы без
        // типа — то есть показывало бы новичку ровно то состояние, из которого
        // редактор и вывели.
        entt::registry& reg = m_scene->Registry();
        GameObject hud = m_scene->CreateEmptyObject("HUD Panel");
        sage::ui::ApplyPreset(*m_scene, hud.Entity(), "Panel");
        {
            sage::ui::Element& xf = reg.get<sage::ui::Element>(hud.Entity());
            xf.Anchor = UIAnchor::TopLeft;
            xf.Position = {16.0f, 16.0f};
            xf.Size = {230.0f, 64.0f};
            sage::ui::Fill& fill = reg.get<sage::ui::Fill>(hud.Entity());
            fill.Rounding = 12.0f;
            fill.BorderThickness = 2.0f;
        }
        // Надпись — ОТДЕЛЬНЫМ ЭЛЕМЕНТОМ внутри панели, как её и строят: панель
        // группирует, текст показывает текст. «Подложка со встроенной надписью»
        // — то, от чего система типов и уходит.
        GameObject title = m_scene->CreateEmptyObject("Title");
        sage::ui::ApplyPreset(*m_scene, title.Entity(), "Text");
        {
            sage::ui::Element& xf = reg.get<sage::ui::Element>(title.Entity());
            xf.Anchor = UIAnchor::TopLeft;
            xf.Position = {12.0f, 8.0f};
            xf.Size = {200.0f, 24.0f};
            sage::ui::Label& lab = reg.get<sage::ui::Label>(title.Entity());
            lab.Text = "SAGE UI";
            lab.Horizontal = sage::ui::Label::Align::Start;
            lab.AutoWidth = false;
        }
        m_scene->SetParent(title.Entity(), hud.Entity());

        GameObject hp = m_scene->CreateEmptyObject("HP Bar");
        sage::ui::ApplyPreset(*m_scene, hp.Entity(), "Bar");
        {
            sage::ui::Element& xf = reg.get<sage::ui::Element>(hp.Entity());
            xf.Anchor = UIAnchor::BottomLeft;   // внутри панели-родителя
            xf.Position = {12.0f, 8.0f};
            xf.Size = {206.0f, 18.0f};
            sage::ui::Fill& fill = reg.get<sage::ui::Fill>(hp.Entity());
            fill.Rounding = 8.0f;
            fill.Color = {0.0f, 0.0f, 0.0f, 0.55f};
            sage::ui::Bar& bar = reg.get<sage::ui::Bar>(hp.Entity());
            bar.Value = 0.72f;
            bar.FillColor = {0.85f, 0.30f, 0.30f, 1.0f};
            bar.Smoothing = 3.0f;
        }
        m_scene->SetParent(hp.Entity(), hud.Entity());
    }

    if (content == ProjectTemplateKind::Demo) {
        // Что-то выбрано сразу — гизмо видно, Inspector не пустой. Выбираем
        // криволинейный примитив: сразу демонстрирует аутлайн на изогнутом
        // силуэте (кайма строится из силуэта меша — точна для любой формы).
        GameObject sel = m_scene->FindByName("Cone");
        if (!sel.Valid()) sel = m_scene->FindByName("Green Cube");
        if (sel.Valid()) m_selection.SetPrimary(sel.Id());
    }
    UpdateWindowTitle();
}

bool EditorLayer::LoadSceneFromFile(const fs::path& path) {
    if (InPlayMode()) StopPlay(); // см. NewScene
    try {
        m_scene = SceneSerializer::Load(path.string());
        m_history.Clear();
            m_selection.SetPrimary(-1);
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
// ============================================================================
//  ОТЧЁТ О ПРОШЛОМ ПАДЕНИИ
//
//  Обработчик падений пишет подробный отчёт (сигнал, стек, версия, открытый
//  проект, последние строки лога) — и до сих пор об этом знал только тот, кто
//  догадался поискать рядом с редактором файл sage-crash-*.txt. То есть отчёт
//  был, а человек, ради которого он писался, его не видел: он видел, что
//  «редактор закрылся сам».
//
//  Показать отчёт В МОМЕНТ падения нельзя: процесс уже разрушен, и рисовать
//  ImGui в нём — верный способ упасть второй раз, потеряв и отчёт. Поэтому в
//  момент падения — короткое системное окно (см. CrashHandler.cpp), а весь
//  текст — здесь, при следующем запуске, когда его есть чем показать.
// ============================================================================
void EditorLayer::DrawCrashReport() {
    if (!m_recovery.HasCrashReport()) return;
    // Одно имя на оба вызова и просьба ОДИН раз — см. DrawRecoveryPrompt.
    const char* const kId = T("Previous session crashed" "###crash-report");
    if (!ImGui::IsPopupOpen(kId)) ImGui::OpenPopup(kId);
    const ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::min(900.0f, vp.x * 0.8f), std::min(620.0f, vp.y * 0.8f)),
                             ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    ImGui::TextWrapped("%s", T("The editor did not close normally last time. Below is the full "
                               "report written at that moment — it is what a developer needs to "
                               "find the cause."));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", m_recovery.CrashReportPath().c_str());
    if (m_recovery.CrashReportCount() > 1)
        ImGui::TextDisabled(T("Reports next to the editor: %d"), (int)m_recovery.CrashReportCount());
    ImGui::Spacing();

    // Текст — в поле ввода только для чтения, а не Text(): так его можно
    // выделить мышью, прокрутить и скопировать кусок, а не только целиком.
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f;
    ImGui::InputTextMultiline("##crashtext", m_recovery.CrashReportText().data(),
                              m_recovery.CrashReportText().size() + 1,
                              ImVec2(-FLT_MIN, -footer), ImGuiInputTextFlags_ReadOnly);

    if (ImGui::Button(T("Copy report"), ImVec2(160, 0))) {
        ImGui::SetClipboardText(m_recovery.CrashReportText().c_str());
        SetStatusMessage(T("Crash report copied to the clipboard"));
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Copy path"), ImVec2(140, 0))) {
        ImGui::SetClipboardText(m_recovery.CrashReportPath().c_str());
        SetStatusMessage(T("Path copied to the clipboard"));
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Close"), ImVec2(120, 0))) {
        // Файл ОСТАЁТСЯ. Отчёт — единственный след падения, и стирать его
        // кнопкой «закрыть» значило бы отнимать у человека возможность его
        // прислать, когда он до этого дойдёт.
        m_recovery.DismissCrashReport();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (EditorTheme::ColoredButton(T("Delete report"), EditorTheme::Role::Danger, ImVec2(150, 0))) {
        std::error_code ec;
        m_recovery.DeleteCrashReport();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorLayer::DrawRecoveryPrompt() {
    if (!m_recovery.HasRecoveryFile()) return;
    // ИМЯ ОКНА — ЦЕЛИКОМ И ОДНО И ТО ЖЕ В ОБОИХ ВЫЗОВАХ, вместе с «###».
    //
    // Здесь стоял OpenPopup(T("Restore scene?")) — БЕЗ «###». По-английски это
    // работало случайно: ImGui считает имя окна хэшем, сбрасывая его на «###»,
    // и у строки без «###» хэш совпадал с хвостом «###Restore scene?». А в
    // переводе OpenPopup получал «Восстановить сцену?» — другой хэш, то есть
    // ОТКРЫВАЛОСЬ ОДНО ОКНО, А РИСОВАЛОСЬ ДРУГОЕ.
    //
    // И это не «диалог не показался». Открытое окно, которое никто не рисует,
    // навсегда остаётся в стеке всплывающих окон ImGui, а раз модалка не
    // нарисована, предложение не сбросить — значит OpenPopup зовётся
    // КАЖДЫЙ КАДР. Каждый такой вызов закрывает всё, что человек открыл выше
    // (ImGui::OpenPopupEx -> ClosePopupToLevel), и меню «Файл» захлопывается в
    // том же кадре, в котором открылось. Снаружи это выглядит как «редактор
    // завис, кнопки не нажимаются»: картинка живая, вьюпорт крутится, а ни
    // одно меню и ни один диалог не открываются — и ни строчки в логе.
    //
    // Ловится это ДВУМЯ сторожами, потому что случай возвращается: одно и то же
    // имя в обоих вызовах проверяет scripts/check_popup_ids.py, а «открыто
    // окно, которое никто не рисует» — проверка кадра (CheckGhostPopups).
    const char* const kId = T("Restore scene?" "###Restore scene?");
    // Просим ОДИН раз, а не каждый кадр: повторный OpenPopup — это удар по
    // чужим открытым окнам, и платить им за то, что наше уже открыто, не за что.
    if (!ImGui::IsPopupOpen(kId)) ImGui::OpenPopup(kId);
    if (ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T("The previous session seems to have crashed"));
        ImGui::Spacing();
        ImGui::Text(T("Found file: %s"), m_recovery.RecoveryFile().c_str());
        ImGui::TextDisabled("%s", T("It did not overwrite your scene — this is a separate copy."));
        ImGui::Spacing();
        if (ImGui::Button(T("Open the copy"), ImVec2(150, 0))) {
            if (LoadSceneFromFile(m_recovery.RecoveryFile())) {
                // Путь сцены НЕ ставим: иначе первое же Ctrl+S записало бы
                // восстановленное поверх файла восстановления, а не сцены.
                m_scenePath.clear();
                m_sceneDirty = true;
                UpdateWindowTitle();
            }
            m_recovery.DismissRecovery();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Keep the file"), ImVec2(150, 0))) {
            m_recovery.DismissRecovery();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Delete"), ImVec2(110, 0))) {
            m_recovery.DeleteRecoveryFile();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// Окно попросили закрыть. Пока сцена не сохранена — не закрываемся, а
// спрашиваем; закроет приложение сам ответ (см. DrawUnsavedPrompt).
//
// Ровно ЗДЕСЬ, а не в кадре: флаг закрытия GLFW ставит в конце кадра, и
// перехват внутри отрисовки не получал управления вовсе — редактор закрывался
// молча вместе с несохранённой сценой (см. Layer::OnCloseRequest).
bool EditorLayer::OnCloseRequest() {
    if (!m_sceneDirty) return true;
    if (!m_unsavedPrompt) {
        m_closeAfterPrompt = true;
        AskUnsaved(nullptr);   // «продолжить» здесь и значит «закрыться»
    }
    return false;
}

// Спросить про несохранённую сцену и, если ответят, сделать action.
//
// Сохранено — спрашивать не о чем, действие идёт сразу: вопрос, который задают
// всегда, перестают читать.
void EditorLayer::AskUnsaved(std::function<void()> action) {
    if (!m_sceneDirty) {
        if (action) action();
        return;
    }
    m_afterUnsaved = std::move(action);
    m_unsavedPrompt = true;
}

// ТРИ ОТВЕТА, А НЕ ДВА. «Сохранить» и «Отмена» — это ловушка для того, кто
// действительно хочет выбросить черновик: он вынужден сохранять мусор поверх
// хорошего файла. Поэтому «Сохранить», «Не сохранять» и «Отмена» — ровно тот
// набор, который человек видел во всех программах, где что-то редактируют.
void EditorLayer::DrawUnsavedPrompt() {
    if (!m_unsavedPrompt) return;
    // Одно имя на оба вызова и просьба ОДИН раз — см. DrawRecoveryPrompt.
    const char* const kId = T("Scene not saved" "###Unsaved");
    if (!ImGui::IsPopupOpen(kId)) ImGui::OpenPopup(kId);
    if (ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string name = m_scenePath.empty() ? CurrentSceneName()
                                                     : m_scenePath.filename().string();
        ImGui::Text(T("Scene \"%s\" has unsaved changes."), name.c_str());
        ImGui::TextDisabled("%s", T("They will be lost if you continue."));
        ImGui::Spacing();

        auto finish = [&](bool proceed) {
            m_unsavedPrompt = false;
            const bool closing = m_closeAfterPrompt;
            m_closeAfterPrompt = false;
            auto action = std::move(m_afterUnsaved);
            m_afterUnsaved = nullptr;
            ImGui::CloseCurrentPopup();
            if (!proceed) return;
            if (action) action();
            if (closing) sage::Application::Get().Close();
        };

        if (ImGui::Button(T("Save"), ImVec2(150, 0))) {
            if (m_scenePath.empty()) {
                // Имени у сцены ещё нет — спрашиваем его, а отложенное действие
                // отменяем: продолжать, не дождавшись записи, значит потерять
                // ровно то, что человек попросил сохранить.
                m_unsavedPrompt = false;
                m_closeAfterPrompt = false;
                m_afterUnsaved = nullptr;
                RequestDialog("Save Scene As");
                ImGui::CloseCurrentPopup();
            } else if (SaveSceneToFile(m_scenePath)) {
                finish(true);
            } else {
                // Запись не удалась — причина в консоли, а выход отменяем:
                // закрыться после неудачной записи значит потерять сцену.
                finish(false);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Don't save"), ImVec2(150, 0))) finish(true);
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(110, 0))) finish(false);
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
        // Обложка снимается ПОСЛЕ сохранения и не в этой функции: кадр
        // существует только в конце кадра (см. TakeSceneShot). Здесь — заказ.
        RequestSceneShot();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Scene save failed: " << e.what();
        return false;
    }
}

// --- Обложка сцены ----------------------------------------------------------
//
// Заказ обложки: путь считается СЕЙЧАС (пока известны и проект, и сцена), а
// снимок берётся в конце кадра — раньше игрового кадра просто нет.
void EditorLayer::RequestSceneShot() {
    m_sceneShotPath.clear();
    if (!m_project.Loaded() || m_scenePath.empty()) return;
    const fs::path cover = scenecover::For(m_project.Dir(), m_scenePath);
    if (cover.empty()) return;
    std::error_code ec;
    fs::create_directories(cover.parent_path(), ec);
    if (ec) return;
    m_sceneShotPath = sage::PathToUtf8(cover);
}

// Снимок сцены в заказанный файл. Зовётся из конца кадра.
//
// ИГРОВОЙ КАДР ПРЕДПОЧТИТЕЛЬНЕЕ вида редактора: в нём нет ни сетки, ни гизмо,
// ни каймы выделения — то есть он показывает СЦЕНУ, а не рабочее место. Если
// игровой камеры в сцене нет, снимаем вьюпорт: обложка с сеткой всё равно
// отвечает на вопрос «какая это сцена» лучше, чем её отсутствие.
void EditorLayer::TakeSceneShot() {
    if (m_sceneShotPath.empty()) return;
    const std::string path = m_sceneShotPath;
    m_sceneShotPath.clear();

    if (!m_renderer.SaveGameFrame(path) && !m_renderer.SaveViewportFrame(path)) {
        LOG_WARN("Editor") << "Обложка сцены не снялась: кадра нет";
        return;
    }
    // Та же картинка — обложкой ПРОЕКТА в стартовом окне (см. SceneCover.h).
    std::error_code ec;
    const fs::path shot = scenecover::ProjectShot(m_project.Dir());
    fs::create_directories(shot.parent_path(), ec);
    fs::copy_file(path, shot, fs::copy_options::overwrite_existing, ec);
    if (ec) LOG_WARN("Editor") << "Обложка проекта не обновилась: " << ec.message();
}

// Заметка шаблона — В КОНСОЛЬ, а не только в диалог.
//
// Из стартового окна проект создают без диалога, и пояснение там показать
// негде; а именно там его и не хватает: витрина строит мир скриптами, и
// созданный проект выглядит пустым. Строка в консоли — то место, куда человек
// смотрит, когда что-то «не работает», и она обязана быть там раньше вопроса.
void EditorLayer::AnnounceTemplateNote(const ProjectTemplate& tpl) {
    m_templateNote.clear();
    if (tpl.Note.empty()) return;
    // Через T(): у встроенного шаблона заметка — ключ перевода, и в консоли она
    // обязана быть на том же языке, что и весь интерфейс. У УСТАНОВЛЕННОГО
    // шаблона перевода взяться неоткуда: там это текст из его template.json,
    // и T() честно вернёт его как есть.
    m_templateNote = sage::editor::T(tpl.Note.c_str());
    LOG_INFO("Editor") << m_templateNote;
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
        if (InPlayMode() && m_play.Scripts()) m_play.Scripts()->UpdateAll(1.0f / 60.0f);
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
    ForgetPreviousProject();
    const ProjectTemplateKind kind = tpl->Kind;
    // ПАНЕЛЬ АССЕТОВ ЖИВЁТ В assets/ И ТОЛЬКО В НЕЙ (см. AssetsPanel.cpp).
    m_assetsCwd = m_project.AssetsDir();
    m_projects.Touch(m_project.Dir().string());

    if (kind == ProjectTemplateKind::Copy) {
        // Копия ГОТОВОГО проекта: сцены, скрипты, модели и настройки — как
        // есть. Имя проекта при этом остаётся тем, которое выбрал человек:
        // копируется содержимое, а не чужая вывеска.
        const fs::path src = ProjectTemplatesRoot() / tpl->SourceDir;
        std::error_code ec;
        for (const fs::directory_entry& e : fs::directory_iterator(src, ec)) {
            // Файл проекта пропускаем — он уже создан с нужным именем.
            if (e.path().filename() == "project.sageproj") continue;
            CopyInto(e.path(), m_project.Dir() / e.path().filename(), ec);
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

    // ШАБЛОН ОБЯЗАН ОСТАВИТЬ ФАЙЛ СЦЕНЫ, а не только картинку в окне.
    //
    // Ровно эта жалоба и привела сюда: «выберу базовый шаблон — сцену
    // показывает, а самого файла сцены НЕТ». Так и было: встроенный шаблон
    // строил сцену в памяти, m_scenePath оставался пустым, и дальше всё
    // ломалось по цепочке — в scenes/ пусто, в панели ассетов сцену не
    // открыть, Ctrl+S спрашивает, куда сохранять, собранная игра не находит
    // с чего начать. Проект без сцены на диске — это не проект.
    //
    // Имя main.sage не случайное: с него начинает плеер (см.
    // runtime/src/PlayerLayer.cpp) и его же ищут шаблоны-копии.
    const fs::path scenePath = m_project.ScenesDir() / "main.sage";
    if (!SaveSceneToFile(scenePath)) {
        err = "Failed to write the first scene: " + sage::PathToUtf8(scenePath);
        return false;
    }

    AnnounceTemplateNote(*tpl);
    UpdateWindowTitle();
    return true;
}

// ============================================================================
//  Раскладка управления проекта (<проект>/input.sageinput)
//
//  Отдельный файл, а не часть sage.cfg: качество картинки настраивает тот, кто
//  играет, а раскладку — тот, кто делает игру. Смешать их значило бы позволить
//  игроку случайно снести управление, поправив яркость.
// ============================================================================

fs::path EditorLayer::ProjectInputFile() const {
    return m_project.Loaded() ? (m_project.Dir() / "input.sageinput") : fs::path();
}

// Переносит раскладку проекта в РАБОТАЮЩИЙ ввод Play-режима.
//
// Через строку, а не копированием объекта: InputSystem некопируем намеренно
// (у него подписки, курсор и шины — то, что принадлежит хозяину, а не
// раскладке). Сериализация же переносит ровно то, что и должно переноситься, —
// контексты, действия, привязки и настройки, — и это тот же путь, которым
// раскладка попадает в собранную игру. Один путь на оба случая: разойдись они,
// расхождение вылезло бы после сборки.
void EditorLayer::ApplyProjectInputMapping() {
    if (m_projectInput.ContextNames().empty()) return;
    bool hasAnyAction = false;
    for (sage::input::Context* ctx : m_projectInput.ContextsByPriority())
        if (!ctx->ActionNames().empty()) hasAnyAction = true;
    if (!hasAnyAction) return;  // пустая раскладка не должна затирать умолчания скриптов

    m_play.Input().LoadMappingFromString(m_projectInput.SaveMappingToString());
}

bool EditorLayer::SaveProjectInput() {
    const fs::path file = ProjectInputFile();
    if (file.empty()) return false;
    if (!m_projectInput.SaveMapping(file)) return false;
    m_projectInputDirty = false;
    LOG_INFO("Editor") << "Раскладка управления сохранена: " << file.string();
    return true;
}

bool EditorLayer::ReloadProjectInput() {
    const fs::path file = ProjectInputFile();
    // Начинаем с чистого листа: Load ЗАМЕЩАЕТ привязки известных действий, но
    // не убирает те, которых в файле нет, — без сброса «перечитать» оставляло
    // бы на экране действия, удалённые из файла руками.
    m_projectInput.ClearActions();
    m_projectInputDirty = false;
    if (file.empty()) return false;
    // Нет файла — не ошибка: у проекта просто ещё нет своей раскладки.
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;
    return m_projectInput.LoadMapping(file);
}

// ============================================================================
//  СМЕНА ПРОЕКТА: ПРОШЛЫЙ ОБЯЗАН ИСЧЕЗНУТЬ ЦЕЛИКОМ
//
//  «Создал частицу в одном проекте, перешёл в другой — и на секунду видно те
//  объекты и ту работу». Это не мерцание отрисовки, а честно оставшееся
//  состояние: смена проекта меняла ПУТЬ, но не трогала ничего из того, что уже
//  загружено и живёт.
//
//  Держалось прошлое в четырёх местах, и каждое по своей причине:
//
//    • КЭШ РЕСУРСОВ хранит модели, текстуры, материалы и шейдеры ПО ССЫЛКЕ
//      ПРОЕКТА («assets/hero.fbx»). Ссылка в каждом проекте своя и указывает на
//      РАЗНЫЙ файл — значит новый проект получал чужую модель просто потому,
//      что имя совпало. Хуже того, выглядит это правдоподобно: модель есть,
//      материал есть, только не те.
//    • ЧАСТИЦЫ живут секундами и системе, которая их родила, ничем не обязаны:
//      залп, пущенный в старом проекте, догорал уже в новом. Стримы — ещё
//      дольше: непрерывный эмиттер продолжал сыпать от объекта, которого в
//      новой сцене нет вовсе.
//    • ОБЛОЖКИ (thumbs и буферы AssetPreview) ключуются путём ассета — та же
//      беда, что у кэша ресурсов, только видна прямо в панели.
//    • СЦЕНА прошлого проекта оставалась на экране до тех пор, пока не
//      загрузится сцена нового. А если сцен в новом проекте нет вовсе — не
//      уходила совсем: человек видел чужие объекты в пустом проекте.
//
//  Зовётся ПОСЛЕ успешного открытия/создания: провал открытия не повод стирать
//  то, над чем человек работает. Проект, который не открылся, — это не смена
//  проекта.
// ============================================================================
void EditorLayer::ForgetPreviousProject() {
    // Сцена — первой и СРАЗУ, а не «когда загрузится новая». Новый проект может
    // оказаться без сцен вовсе, и тогда «когда загрузится» не наступает.
    // NewScene, а не присваивание: он же сбрасывает историю правок, выделение,
    // путь сцены и признак изменённости — всё это тоже прошлое.
    NewScene(ProjectTemplateKind::Empty);

    ResourceManager::Instance().ForgetProjectAssets();
    m_renderer.Particles().Clear();
    thumbs::Clear();
    sage::scene::ClearPrefabCache();
    m_assets.ForgetProject();
    m_inspector.ForgetProject();
    m_uiInspector.ForgetProject();
    m_nineSlice.ForgetProject();
    // Ссылки на сущности прошлой сцены: их номера в новой сцене принадлежат
    // другим объектам, и «выделен объект 7» после смены проекта значит
    // выделенным чужой.
    m_selection.Clear();
}

bool EditorLayer::OpenProject(const std::string& path, std::string& err) {
    if (!m_project.Open(path, err)) return false;
    ForgetPreviousProject();
    // ПАНЕЛЬ АССЕТОВ ЖИВЁТ В assets/ И ТОЛЬКО В НЕЙ (см. AssetsPanel.cpp).
    m_assetsCwd = m_project.AssetsDir();
    m_projects.Touch(m_project.Dir().string());

    // Настройки проекта (sage.cfg) — в окно Settings; отсутствие файла не ошибка
    // (остаются значения по умолчанию).
    m_settings = sage::EngineConfig{};
    m_settings.LoadFile((m_project.Dir() / "sage.cfg").string());
    m_settings.ApplyEnvOverrides();   // SAGE_* поверх файла — как у рантайма
    ApplyEngineSettings();

    // Раскладка управления проекта — в окно «Управление»; отсутствие файла не
    // ошибка (у проекта её просто ещё нет).
    ReloadProjectInput();

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

// ЕСТЬ ЛИ У ПРОЕКТА ХОТЬ ОДНА СЦЕНА.
//
// Отдельной функцией, потому что вопрос задают двое: сама сборка (она без
// сцены отказывается) и окно сборки (оно говорит об этом ДО нажатия, а не
// после). Два ответа на один вопрос однажды разойдутся — и окно будет
// обещать то, чего сборка не сделает.
bool EditorLayer::HasAnyScene() const {
    if (!m_project.Loaded()) return false;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(m_project.ScenesDir(), ec)) {
        if (entry.path().extension() == ".sage") return true;
    }
    return false;
}

bool EditorLayer::BuildGame(const fs::path& outputDir, std::string& err) {
    if (!m_project.Loaded()) {
        err = "No project open";
        return false;
    }

    // ИГРЫ БЕЗ ЕДИНОЙ СЦЕНЫ НЕ БЫВАЕТ, и собирать её незачем.
    //
    // Плеер при запуске открывает сцену проекта; сцен нет — он показывает
    // пустой экран, и человек, получивший такую сборку, видит чёрное окно без
    // единого объяснения. Хуже того, сборка при этом проходит УСПЕШНО:
    // пятьдесят мегабайт файлов, зелёная строка «готово» — и нерабочая игра.
    //
    // Пустая папка scenes/ и её отсутствие — одно и то же: и там и там сцены
    // нет. Отказ здесь стоит секунды, а разбирательство «почему собранная игра
    // чёрная» — вечера.
    if (!HasAnyScene()) {
        err = T("The project has no scene: there is nothing to run. Create or save a scene "
                "into scenes/ first.");
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
        // МАНИФЕСТ ЛЕЖИТ В ПАКЕТЕ, а не россыпью рядом с exe.
        //
        // project.sageproj — это манифест игры: по нему плеер узнаёт её имя.
        // Раньше он копировался ОТДЕЛЬНЫМ файлом рядом с exe, и получалось,
        // что игра, целиком упакованная в один game.sagepak, всё равно везёт
        // рядом кусок редакторского проекта — тот самый, который у автора уже
        // есть. Теперь он внутри пакета, и плеер читает его оттуда (см.
        // runtime/src/PlayerLayer.cpp).
        //
        // Файлы .meta и .sageimport в пакет не кладутся: это служебные данные
        // редактора (GUID'ы ассетов, параметры импорта), в игре по ним никто не
        // ходит, а место они занимают.
        const size_t packed = pack.AddDirectory(m_project.Dir(), {".meta", ".sageimport"});
        if (!pack.Save(gameDir / "game.sagepak")) {
            err = T("Could not write the game package");
            return false;
        }

        // ОТДЕЛЬНАЯ КОПИЯ — только если её попросили параметром сборки (см.
        // EngineConfig::BuildProjectFile; в настройках редактора его нет и не
        // должно быть). Нужна она ровно для одного: запускать игру, перетащив
        // project.sageproj на плеер.
        if (m_settings.BuildProjectFile) {
            fs::copy_file(m_project.Dir() / "project.sageproj", gameDir / "project.sageproj",
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                err = T("Could not copy the project file: ") + ec.message();
                return false;
            }
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

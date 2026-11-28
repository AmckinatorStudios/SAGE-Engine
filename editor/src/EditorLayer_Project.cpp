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

void EditorLayer::NewScene() {
    if (InPlayMode()) StopPlay(); // нельзя подменять сцену под работающими скриптами
    // Новый уровень встаёт на место УРОВНЯ: документ интерфейса — в сторону.
    if (m_uiDocActive) SetWorkspace(EditorWorkspace::Scene);
    m_history.Clear();
    m_scene = std::make_unique<Scene>("Untitled");
    m_selection.SetPrimary(-1);
    m_scenePath.clear();
    m_sceneDirty = false;

    UpdateWindowTitle();
}

// Новая сцена ДЛЯ ЧЕЛОВЕКА — сразу с объектом-солнцем.
//
// Пустая сцена теперь и правда пустая: света в ней нет вовсе (см.
// LightingEnvironment::Sun — поле окружения больше не горит само собой). Без
// объекта-солнца человек, выбравший «Новая сцена», получил бы чёрный вьюпорт и
// ни одной подсказки, что делать. Солнце заводится ОБЪЕКТОМ: его видно в
// иерархии, его можно выбрать, повернуть гизмо и удалить — и вот тогда
// темнота будет объяснимой, потому что удалил её сам.
//
// Отдельно от NewScene намеренно: NewScene() — чистый лист для кода и
// самопроверки, и класть туда объекты значит менять смысл «пусто».
void EditorLayer::NewSceneForUser() {
    NewScene();
    sage::ecs::CreateSunEntity(*m_scene);
    // Выделение НЕ трогаем: смена проекта обязана оставлять его пустым (номера
    // объектов прошлой сцены принадлежат в новой другим), и солнце,
    // выделенное «за компанию», выглядело бы как несброшенное выделение.
    m_sceneDirty = false; // только что созданная сцена ещё не правлена
}

bool EditorLayer::LoadSceneFromFile(const fs::path& path) {
    if (InPlayMode()) StopPlay(); // см. NewScene
    if (m_uiDocActive) SetWorkspace(EditorWorkspace::Scene);
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


bool EditorLayer::SaveSceneToFile(const fs::path& path) {
    // Посмотренный твин в файл не уезжает: вернуть значения до записи.
    m_tween.StopPreview(*this);
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

bool EditorLayer::CreateProject(const std::string& dir, const std::string& name, std::string& err) {
    if (!m_project.CreateNew(dir, name, err)) return false;
    ForgetPreviousProject();
    // ПАНЕЛЬ АССЕТОВ ЖИВЁТ В assets/ И ТОЛЬКО В НЕЙ (см. AssetsPanel.cpp).
    m_assetsCwd = m_project.AssetsDir();
    m_projects.Touch(m_project.Dir().string());

    // Новый проект — пустой ПРОЕКТ, а не чистый лист для кода: сцену в нём
    // открывает человек, и она обязана быть освещена своим солнцем.
    NewSceneForUser();

    // ПРОЕКТ ОБЯЗАН ОСТАВИТЬ ФАЙЛ СЦЕНЫ, а не только картинку в окне: без него
    // в scenes/ пусто, Ctrl+S спрашивает, куда сохранять, а собранная игра не
    // находит, с чего начать. Имя main.sage не случайное: с него начинает
    // плеер (см. runtime/src/PlayerLayer.cpp).
    const fs::path scenePath = m_project.ScenesDir() / "main.sage";
    if (!SaveSceneToFile(scenePath)) {
        err = "Failed to write the first scene: " + sage::PathToUtf8(scenePath);
        return false;
    }
    UpdateWindowTitle();
    return true;
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
    // Документ интерфейса — тоже. Без сохранения: проект уже сменился, и
    // ссылка на файл разрешилась бы в НОВЫЙ проект — старый интерфейс лёг бы
    // поверх чужого файла.
    CloseInterfaceDocument(/*save=*/false);
    NewScene();

    ResourceManager::Instance().ForgetProjectAssets();
    m_renderer.Particles().Clear();
    thumbs::Clear();
    sage::scene::ClearPrefabCache();
    m_assets.ForgetProject();
    m_inspector.ForgetProject();
    m_uiInspector.ForgetProject();
    m_spriteSheet.ForgetProject();
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

    // Открытый проект сразу показывает свой контент, а не осиротевшую демо-сцену.
    // Открывается СТАРТОВАЯ сцена — та, с которой начнётся игра: человек,
    // открывший проект, чаще всего идёт править именно её, и показывать вместо
    // неё случайную первую по алфавиту значит каждый раз искать нужную руками.
    std::error_code ec;
    fs::path opened;
    if (!m_project.StartScene().empty()) {
        const fs::path start = m_project.ScenesDir() / (m_project.StartScene() + ".sage");
        if (fs::exists(start, ec)) opened = start;
    }
    if (opened.empty()) {
        std::vector<fs::path> scenes;
        for (const auto& entry : fs::directory_iterator(m_project.ScenesDir(), ec)) {
            if (entry.path().extension() == ".sage") scenes.push_back(entry.path());
        }
        std::sort(scenes.begin(), scenes.end());
        if (!scenes.empty()) opened = scenes.front();
    }
    if (!opened.empty()) LoadSceneFromFile(opened);

    UpdateWindowTitle();
    return true;
}


// Заголовок OS-окна: "SAGE Editor — сцена[*] — проект". Обновляется только
// по факту изменения (не дёргаем GLFW каждый кадр).
void EditorLayer::UpdateWindowTitle() {
    std::string scene = m_scenePath.empty() ? m_scene->Name() : m_scenePath.filename().string();
    // В пространстве «Интерфейс» на месте сцены — документ: заголовок о нём.
    if (m_uiDocActive)
        scene = m_uiDocPath.empty() ? m_scene->Name() + ".sageui"
                                    : std::filesystem::path(sage::PathFromUtf8(m_uiDocPath)).filename().string();
    std::string title = "SAGE Editor — " + scene + (m_sceneDirty ? "*" : "");
    if (m_project.Loaded()) title += " — " + m_project.Name();
    if (title == m_windowTitle) return;
    m_windowTitle = title;
    glfwSetWindowTitle(sage::Application::Get().GetWindow().Handle(), title.c_str());
}


// УДАЛИЛИ ФАЙЛЫ — ОТПУСТИТЬ ССЫЛКИ НА НИХ (см. EditorHost::AssetsDeleted).
//
// Удаление всех сцен проекта оставляло редактор держаться за пустоту: путь
// открытой сцены вёл в удалённый файл (Ctrl+S молча создавал его снова),
// project.sageproj называл стартовой сцену, которой нет, а база ассетов
// продолжала выдавать GUID удалённых файлов.
void EditorLayer::AssetsDeleted(const std::vector<fs::path>& paths) {
    auto norm = [](const fs::path& p) {
        std::error_code ec;
        const fs::path a = fs::absolute(p, ec);
        return (ec ? p : a).lexically_normal();
    };
    // p совпадает с root или лежит внутри него (удалили папку со сценой).
    auto within = [&](const fs::path& p, const fs::path& root) {
        const fs::path a = norm(p), r = norm(root);
        auto ai = a.begin();
        for (auto ri = r.begin(); ri != r.end(); ++ri, ++ai) {
            if (ri->empty()) continue;   // хвостовой «/» папки
            if (ai == a.end() || *ai != *ri) return false;
        }
        return true;
    };
    bool sceneGone = false, startGone = false;
    for (const fs::path& victim : paths) {
        if (!m_scenePath.empty() && within(m_scenePath, victim)) sceneGone = true;
        if (m_project.Loaded() && !m_project.StartScene().empty() &&
            within(m_project.ScenesDir() / sage::PathFromUtf8(m_project.StartScene() + ".sage"), victim))
            startGone = true;
    }
    if (sceneGone) {
        LOG_WARN("Editor") << "Файл открытой сцены удалён: " << sage::PathToUtf8(m_scenePath)
                           << " — сцена осталась открытой без файла";
        m_scenePath.clear();
        m_sceneDirty = true;
        SetStatusMessage(T("The scene file was deleted: the scene stays open, save it to keep it"));
        UpdateWindowTitle();
    }
    if (startGone) {
        std::string err;
        if (!m_project.SetStartScene("", err)) LOG_WARN("Editor") << "Стартовая сцена не снята: " << err;
    }
    if (m_project.Loaded()) {
        auto& database = sage::AssetDatabase::Instance();
        database.ScanProject(m_project.Dir().string());
        database.ForgetMissingProjectFiles();
    }
}

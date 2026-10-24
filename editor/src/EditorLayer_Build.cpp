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

// Сборка игры: проверка проекта перед сборкой и запуск GameBuilder.
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


bool EditorLayer::PrepareBuild(const fs::path& outputDir, GameBuilder::Request& req,
                               std::string& err) {
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

    // СТАРТОВАЯ СЦЕНА ОБЯЗАНА СУЩЕСТВОВАТЬ.
    //
    // Названа в проекте, а файла нет — игра запустится НЕ С ТОЙ сцены
    // (запасной путь плеера: main.sage, иначе первая по алфавиту) и будет
    // выглядеть сломанной. Сборка проходит при этом успешно, и связать одно с
    // другим потом почти нечем: отказ здесь стоит секунды.
    if (!m_project.StartScene().empty()) {
        std::error_code startEc;
        const fs::path start = m_project.ScenesDir() / (m_project.StartScene() + ".sage");
        if (!fs::exists(start, startEc)) {
            err = T("The project's start scene is missing: ") + start.filename().string();
            return false;
        }
    } else {
        // Не отказ: игра соберётся и запустится по прежнему правилу. Но сказать
        // об этом надо — «почему запускается не тот уровень» иначе выясняют
        // перебором.
        LOG_WARN("Editor") << "Стартовая сцена проекта не задана — собранная игра возьмёт "
                              "scenes/main.sage, а если её нет, первую по алфавиту. "
                              "Назначить: ПКМ по сцене в панели Assets.";
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

    req.ProjectDir = m_project.Dir();
    req.ProjectName = m_project.Name();
    req.OutputDir = outputDir;
    req.Player = player;
    req.CopyProjectFile = m_settings.BuildProjectFile;
    // Оглавление базы — снимок ЗДЕСЬ, в главном потоке: база живёт в нём, и
    // читать её из потока сборки, пока редактор её правит, нельзя.
    req.AssetIndex = sage::AssetDatabase::Instance().ExportIndex();
    return true;
}


bool EditorLayer::BuildGame(const fs::path& outputDir, std::string& err) {
    GameBuilder::Request req;
    if (!PrepareBuild(outputDir, req, err)) return false;
    return GameBuilder::RunSync(req, err);
}


bool EditorLayer::StartBuildGame(const fs::path& outputDir, std::string& err) {
    GameBuilder::Request req;
    if (!PrepareBuild(outputDir, req, err)) return false;
    if (!m_builder.Start(req)) {
        err = T("A build is already running");
        return false;
    }
    return true;
}

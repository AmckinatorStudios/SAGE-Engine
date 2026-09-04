// ============================================================================
//  sage — командная строка движка.
//
//  ЗАЧЕМ. Проект до сих пор мог создать только редактор. Значит разработчик,
//  который пишет игру кодом (а движок это позволяет — см. games/), проектом
//  воспользоваться не мог вовсе: ни создать, ни узнать, что в нём лежит. Чтобы
//  получить открываемый редактором проект, приходилось запускать редактор —
//  то есть редактор оказывался обязательным звеном там, где он не нужен.
//
//  Теперь так:
//
//      sage project create МояИгра
//      sage project info МояИгра
//      sage scene create МояИгра Уровень2
//      SageEditor МояИгра/project.sageproj      # тот же проект, без конвертации
//
//  И наоборот: проект, созданный редактором, эта программа читает как свой,
//  потому что читает его ТЕМ ЖЕ классом (sage::project::Project). Никакого
//  «экспорта», «импорта» и второй копии проекта не существует — их негде
//  завести, реализация одна.
//
//  ПОЧЕМУ БЕЗ ОКНА И БЕЗ ГРАФИКИ. Инструмент должен работать на сборочной
//  машине, по ssh и в git-хуке. Он линкует sage::engine, но не создаёт
//  Application — движок этого не требует, и это ровно то свойство, ради
//  которого системы движка не завязаны на существование окна.
// ============================================================================
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sage/core/Version.h"
#include "sage/project/Project.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

namespace fs = std::filesystem;

namespace {

int Usage() {
    std::printf(
        "sage — командная строка SAGE Engine v%s\n"
        "\n"
        "  sage project create <имя> [--dir <куда>] [--scene <имя сцены>]\n"
        "        Создаёт проект: project.sageproj, scenes/, assets/ и первую сцену.\n"
        "        Полученную папку открывает SageEditor — без конвертации.\n"
        "\n"
        "  sage project info [<путь>]\n"
        "        Что за проект и что в нём есть. Без пути ищет project.sageproj\n"
        "        в текущем каталоге и выше.\n"
        "\n"
        "  sage project set-start-scene <имя> [--project <путь>]\n"
        "        С какой сцены запускается игра. Это же значение читает плеер.\n"
        "\n"
        "  sage scene create <имя> [--project <путь>]\n"
        "        Добавляет пустую сцену в проект.\n"
        "\n"
        "  sage scene list [--project <путь>]\n"
        "        Сцены проекта в том порядке, в каком их видит движок.\n",
        kSageEngineVersion);
    return 2;
}

// Значение именованного аргумента: --dir X. Пусто — не задан.
std::string Option(const std::vector<std::string>& args, const char* name) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) return args[i + 1];
    }
    return {};
}

// Позиционные аргументы (всё, что не «--имя значение»).
std::vector<std::string> Positional(const std::vector<std::string>& args) {
    std::vector<std::string> out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i].rfind("--", 0) == 0) {
            ++i;  // пропускаем и значение
            continue;
        }
        out.push_back(args[i]);
    }
    return out;
}

// Открывает проект по явному пути или ищет его вверх по дереву — так же, как
// это делают системы контроля версий: человек находится где-то внутри проекта
// и не обязан помнить, где его корень.
bool OpenProject(sage::project::Project& project, const std::string& hint) {
    std::string error;
    if (!hint.empty()) {
        if (project.Open(hint, error)) return true;
        std::fprintf(stderr, "Ошибка: %s\n", error.c_str());
        return false;
    }
    const fs::path found = sage::project::Project::Find(fs::current_path());
    if (found.empty()) {
        std::fprintf(stderr,
                     "Ошибка: project.sageproj не найден ни здесь, ни выше по дереву.\n"
                     "Укажите путь явно или создайте проект: sage project create <имя>\n");
        return false;
    }
    if (project.Open(found, error)) return true;
    std::fprintf(stderr, "Ошибка: %s\n", error.c_str());
    return false;
}

// Пустая сцена на диск. Через SceneSerializer, а не «напишем json руками»:
// формат сцены принадлежит движку, и вторая точка, которая его пишет, разошлась
// бы с первой при первом же изменении формата.
bool WriteEmptyScene(const fs::path& path, const std::string& name, std::string& error) {
    try {
        Scene scene(name);
        SceneSerializer::Save(scene, path.string());
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

int CmdProjectCreate(const std::vector<std::string>& args) {
    const std::vector<std::string> pos = Positional(args);
    if (pos.empty()) return Usage();

    const std::string name = pos[0];
    const std::string dirOpt = Option(args, "--dir");
    const fs::path baseDir = dirOpt.empty() ? fs::current_path() : fs::path(dirOpt);
    std::string sceneName = Option(args, "--scene");
    if (sceneName.empty()) sceneName = "main";

    sage::project::Project project;
    std::string error;
    if (!project.Create(baseDir, name, error)) {
        std::fprintf(stderr, "Ошибка: %s\n", error.c_str());
        return 1;
    }

    // Проект без единой сцены формально корректен, но запустить его нечем, и
    // первое, что человек сделает, — создаст сцену. Делаем это за него.
    const fs::path scenePath = project.ScenePath(sceneName);
    if (!WriteEmptyScene(scenePath, sceneName, error)) {
        std::fprintf(stderr, "Ошибка: сцену записать не удалось: %s\n", error.c_str());
        return 1;
    }
    project.MutableInfo().StartScene = scenePath.filename().string();
    if (!project.Save(error)) {
        std::fprintf(stderr, "Ошибка: %s\n", error.c_str());
        return 1;
    }

    std::printf("Проект создан: %s\n", project.Dir().string().c_str());
    std::printf("  %s\n", project.ProjectFile().filename().string().c_str());
    std::printf("  scenes/%s\n", scenePath.filename().string().c_str());
    std::printf("  assets/\n");
    std::printf("\nОткрыть в редакторе:  SageEditor %s\n",
                project.ProjectFile().string().c_str());
    return 0;
}

int CmdProjectInfo(const std::vector<std::string>& args) {
    const std::vector<std::string> pos = Positional(args);
    sage::project::Project project;
    if (!OpenProject(project, pos.empty() ? std::string{} : pos[0])) return 1;

    const sage::project::Info& info = project.GetInfo();
    std::printf("Проект:        %s\n", info.Name.c_str());
    std::printf("Папка:         %s\n", project.Dir().string().c_str());
    std::printf("Формат:        версия %d\n", info.FormatVersion);
    if (!info.EngineVersion.empty())
        std::printf("Создан в:      SAGE %s\n", info.EngineVersion.c_str());

    const std::vector<std::string> scenes = project.SceneNames();
    std::printf("Сцен:          %zu\n", scenes.size());
    const std::string start = project.StartSceneName();
    for (const std::string& scene : scenes) {
        std::printf("  %s%s\n", scene.c_str(), scene == start ? "   <- стартовая" : "");
    }
    if (scenes.empty()) {
        std::printf("  (ни одной — добавьте: sage scene create <имя>)\n");
    }
    if (!info.StartScene.empty() && info.StartScene != start) {
        // Поле есть, но указывает на несуществующую сцену. Промолчать значило бы
        // оставить человека с уверенностью, что игра запустится не с той сцены,
        // с какой она запустится на самом деле.
        std::printf("ВНИМАНИЕ:      start_scene = «%s», но такой сцены нет\n",
                    info.StartScene.c_str());
    }
    return 0;
}

int CmdProjectSetStartScene(const std::vector<std::string>& args) {
    const std::vector<std::string> pos = Positional(args);
    if (pos.empty()) return Usage();

    sage::project::Project project;
    if (!OpenProject(project, Option(args, "--project"))) return 1;

    const std::string wanted = pos[0];
    const std::vector<std::string> scenes = project.SceneNames();
    const fs::path path = project.ScenePath(wanted);
    const std::string file = path.filename().string();
    bool exists = false;
    for (const std::string& scene : scenes) {
        if (scene == file) exists = true;
    }
    if (!exists) {
        std::fprintf(stderr, "Ошибка: сцены «%s» в проекте нет.\n", file.c_str());
        std::fprintf(stderr, "Есть: ");
        for (const std::string& scene : scenes) std::fprintf(stderr, "%s ", scene.c_str());
        std::fprintf(stderr, "\n");
        return 1;
    }

    project.MutableInfo().StartScene = file;
    std::string error;
    if (!project.Save(error)) {
        std::fprintf(stderr, "Ошибка: %s\n", error.c_str());
        return 1;
    }
    std::printf("Стартовая сцена: %s\n", file.c_str());
    return 0;
}

int CmdSceneCreate(const std::vector<std::string>& args) {
    const std::vector<std::string> pos = Positional(args);
    if (pos.empty()) return Usage();

    sage::project::Project project;
    if (!OpenProject(project, Option(args, "--project"))) return 1;

    const fs::path path = project.ScenePath(pos[0]);
    std::error_code ec;
    if (fs::exists(path, ec)) {
        std::fprintf(stderr, "Ошибка: сцена уже есть: %s\n", path.string().c_str());
        return 1;
    }
    std::string error;
    if (!WriteEmptyScene(path, path.stem().string(), error)) {
        std::fprintf(stderr, "Ошибка: %s\n", error.c_str());
        return 1;
    }
    std::printf("Сцена создана: %s\n", path.string().c_str());
    return 0;
}

int CmdSceneList(const std::vector<std::string>& args) {
    sage::project::Project project;
    if (!OpenProject(project, Option(args, "--project"))) return 1;

    const std::string start = project.StartSceneName();
    for (const std::string& scene : project.SceneNames()) {
        std::printf("%s%s\n", scene.c_str(), scene == start ? "   <- стартовая" : "");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return Usage();

    const std::string group = argv[1];
    const std::string command = argv[2];
    const std::vector<std::string> args(argv + 3, argv + argc);

    if (group == "project") {
        if (command == "create") return CmdProjectCreate(args);
        if (command == "info") return CmdProjectInfo(args);
        if (command == "set-start-scene") return CmdProjectSetStartScene(args);
    } else if (group == "scene") {
        if (command == "create") return CmdSceneCreate(args);
        if (command == "list") return CmdSceneList(args);
    }
    return Usage();
}

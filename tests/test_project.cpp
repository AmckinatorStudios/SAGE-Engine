// ============================================================================
//  Проект как понятие ДВИЖКА: один класс на SDK, редактор и плеер.
//
//  ЧТО ЗДЕСЬ ПРОВЕРЯЕТСЯ И ПОЧЕМУ. Раньше «проект» существовал в двух
//  реализациях: класс в редакторе и ручной разбор project.sageproj в плеере,
//  со своим правилом поиска главной сцены. Разойтись им было достаточно одного
//  нового поля в файле проекта: редактор начал бы его писать, плеер — молча не
//  замечать. Из кода игры проект не был доступен вовсе.
//
//  Теперь реализация одна, и эти проверки закрепляют её свойства — те самые,
//  на которых держится совместная работа SDK и редактора над одним каталогом:
//
//    • созданный программно проект ОТКРЫВАЕТСЯ как есть, без конвертации;
//    • правило «какая сцена стартовая» одно, задаётся автором игры и
//      одинаково отвечает и редактору, и плееру;
//    • ссылки на ассеты пишутся относительно корня проекта — иначе собранная
//      игра не найдёт ни одного файла на чужой машине.
//
//  Всё без окна и без графического контекста: проект — это данные, и требовать
//  ради него запущенного приложения было бы ровно той ошибкой, которую мы и
//  чиним.
// ============================================================================
#include "TestFramework.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "sage/project/Project.h"
#include "sage/project/ProjectWatcher.h"

namespace fs = std::filesystem;
using sage::project::Project;

namespace {

// Отдельная пустая папка на каждую проверку: проекты трогают базу ассетов и
// имя для сохранений, и общая папка сделала бы порядок проверок значимым.
fs::path FreshDir(const std::string& tag) {
    const fs::path dir = fs::temp_directory_path() / ("sage-project-test-" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void WriteScene(const Project& project, const std::string& file) {
    std::ofstream out(project.ScenesDir() / file);
    out << R"({"name":"Проверка","sage_scene_version":5,"objects":[]})";
}

} // namespace

TEST(Project_СозданиеДаётСразуОткрываемыйПроект) {
    const fs::path base = FreshDir("create");
    Project created;
    std::string error;
    CHECK_TRUE(created.Create(base, "МояИгра", error));

    // Раскладка на диске — та, которую ждут и редактор, и плеер.
    CHECK_TRUE(fs::exists(base / "МояИгра" / "project.sageproj"));
    CHECK_TRUE(fs::is_directory(base / "МояИгра" / "scenes"));
    CHECK_TRUE(fs::is_directory(base / "МояИгра" / "assets"));

    // ГЛАВНОЕ: то, что создал код, открывается БЕЗ КОНВЕРТАЦИИ тем же классом,
    // которым пользуется редактор. Отдельный экземпляр, а не тот же самый:
    // проверяем файл на диске, а не поле в памяти.
    Project reopened;
    CHECK_TRUE(reopened.Open(base / "МояИгра", error));
    CHECK_EQ(reopened.Name(), std::string("МояИгра"));
    CHECK_EQ(reopened.GetInfo().FormatVersion, 1);
    // Версия движка записана — по ней через год будет видно, на чём писали.
    CHECK_FALSE(reopened.GetInfo().EngineVersion.empty());

    // Открыть можно и по пути к самому файлу, а не только к папке: именно так
    // проект попадает в редактор перетаскиванием.
    Project byFile;
    CHECK_TRUE(byFile.Open(base / "МояИгра" / "project.sageproj", error));
    CHECK_EQ(byFile.Name(), std::string("МояИгра"));
}

TEST(Project_ИмяСКириллицейПереживаетЗаписьИЧтение) {
    // У человека, который скачает движок, имя проекта запросто написано
    // по-русски. Ровно на кодировке уже ломался запуск редактора (см.
    // test_paths.cpp), и повторять это в файле проекта незачем.
    const fs::path base = FreshDir("unicode");
    Project created;
    std::string error;
    CHECK_TRUE(created.Create(base, "Война Наций", error));

    Project reopened;
    CHECK_TRUE(reopened.Open(base / "Война Наций", error));
    CHECK_EQ(reopened.Name(), std::string("Война Наций"));
}

TEST(Project_ПовторноеСозданиеВНепустойПапкеОтбивается) {
    const fs::path base = FreshDir("occupied");
    Project first;
    std::string error;
    CHECK_TRUE(first.Create(base, "Игра", error));

    // Молча перезаписать чужую папку — потерять чужую работу.
    Project second;
    CHECK_FALSE(second.Create(base, "Игра", error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(second.Loaded());
}

TEST(Project_ОткрытиеНесуществующегоНеЛомается) {
    Project project;
    std::string error;
    CHECK_FALSE(project.Open(fs::temp_directory_path() / "нет-такого-проекта-12345", error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(project.Loaded());
}

TEST(Project_СтартоваяСценаОдноПравилоНаВсех) {
    const fs::path base = FreshDir("startscene");
    Project project;
    std::string error;
    CHECK_TRUE(project.Create(base, "Игра", error));

    // Сцен нет — стартовой нет, и это не ошибка: пустой проект законен.
    CHECK_TRUE(project.StartSceneName().empty());
    CHECK_TRUE(project.StartScenePath().empty());
    CHECK_FALSE(project.HasScenes());

    // Одна сцена — она и стартовая, как бы её ни звали.
    WriteScene(project, "beta.sage");
    CHECK_EQ(project.StartSceneName(), std::string("beta.sage"));

    // Появилась main.sage — соглашение сильнее алфавита. Раньше это правило
    // знал ТОЛЬКО плеер, а редактор брал первую по алфавиту: человек правил
    // одну сцену, а запускал игру с другой.
    WriteScene(project, "main.sage");
    CHECK_EQ(project.StartSceneName(), std::string("main.sage"));

    // Явный выбор автора игры сильнее соглашения.
    project.MutableInfo().StartScene = "beta";   // без расширения — тоже понимаем
    CHECK_EQ(project.StartSceneName(), std::string("beta.sage"));
    CHECK_TRUE(project.Save(error));

    // И этот выбор ДОЕЗЖАЕТ через файл до того, кто откроет проект следующим —
    // то есть до плеера. Ради этого поле и заведено.
    Project reopened;
    CHECK_TRUE(reopened.Open(base / "Игра", error));
    CHECK_EQ(reopened.StartSceneName(), std::string("beta.sage"));

    // Указана несуществующая — не падаем и не запускаем пустоту, а честно
    // откатываемся на соглашение.
    reopened.MutableInfo().StartScene = "которой-нет";
    CHECK_EQ(reopened.StartSceneName(), std::string("main.sage"));
}

TEST(Project_СсылкаНаСценуГодитсяИДляДискаИДляПакета) {
    const fs::path base = FreshDir("sceneref");
    Project project;
    std::string error;
    CHECK_TRUE(project.Create(base, "Игра", error));
    WriteScene(project, "main.sage");

    // Ссылка — всегда относительная и через прямой слэш: в таком виде её
    // понимает и файл сцены, и пакет собранной игры, где диска нет вовсе.
    CHECK_EQ(project.SceneRef("main"), std::string("scenes/main.sage"));
    CHECK_EQ(project.SceneRef("main.sage"), std::string("scenes/main.sage"));
    CHECK_EQ(project.StartSceneRef(), std::string("scenes/main.sage"));
    CHECK_TRUE(project.StartSceneRef().find('\\') == std::string::npos);

    // А путь — полный, для тех, кто работает с диском.
    CHECK_TRUE(project.StartScenePath().is_absolute());
    CHECK_TRUE(fs::exists(project.StartScenePath()));
}

TEST(Project_СписокСценОтсортированИНеЗависитОтФС) {
    const fs::path base = FreshDir("scenelist");
    Project project;
    std::string error;
    CHECK_TRUE(project.Create(base, "Игра", error));
    for (const char* name : {"zulu.sage", "alpha.sage", "mike.sage"}) WriteScene(project, name);
    // Не-сцена в той же папке в список не попадает.
    std::ofstream(project.ScenesDir() / "заметки.txt") << "не сцена";

    const std::vector<std::string> scenes = project.SceneNames();
    CHECK_EQ(scenes.size(), (size_t)3);
    // Порядок обхода каталога системой не определён. Без сортировки «первая
    // сцена» означала бы разное на разных машинах — а с ней и то, какая сцена
    // запустится у игрока.
    CHECK_EQ(scenes[0], std::string("alpha.sage"));
    CHECK_EQ(scenes[1], std::string("mike.sage"));
    CHECK_EQ(scenes[2], std::string("zulu.sage"));
}

TEST(Project_СсылкаНаАссетОтносительнаКорню) {
    const fs::path base = FreshDir("assetref");
    Project project;
    std::string error;
    CHECK_TRUE(project.Create(base, "Игра", error));

    // Файл внутри проекта — ссылка относительная и через прямой слэш. Иначе
    // собранная игра ищет абсолютный путь с чужой машины и не находит ничего.
    const fs::path inside = project.AssetsDir() / "models" / "герой.obj";
    CHECK_EQ(project.AssetRef(inside), std::string("assets/models/герой.obj"));

    // Файл ВНЕ проекта возвращается как есть: сослаться на модель в «Загрузках»
    // — законное временное состояние, и подменять его выдуманным относительным
    // путём было бы враньём.
    const fs::path outside = fs::temp_directory_path() / "чужая" / "модель.obj";
    const std::string ref = project.AssetRef(outside);
    CHECK_TRUE(ref.rfind("assets/", 0) != 0);
}

TEST(Project_ПоискПроектаВверхПоДереву) {
    const fs::path base = FreshDir("find");
    Project project;
    std::string error;
    CHECK_TRUE(project.Create(base, "Игра", error));

    // Человек находится глубоко внутри проекта и не обязан помнить, где корень:
    // так же ведут себя системы контроля версий, и инструмент командной строки
    // обязан вести себя так же.
    const fs::path deep = base / "Игра" / "assets" / "models" / "враги";
    std::error_code ec;
    fs::create_directories(deep, ec);

    const fs::path found = Project::Find(deep);
    CHECK_FALSE(found.empty());
    CHECK_EQ(found.filename().string(), std::string("project.sageproj"));
    CHECK_EQ(found.parent_path().filename().string(), std::string("Игра"));

    // Снаружи проекта — честно не находим, а не отдаём случайный.
    CHECK_TRUE(Project::LooksLikeProject(base / "Игра"));
    CHECK_FALSE(Project::LooksLikeProject(base));
}

TEST(Project_ВотчерВидитПравкуСнаружиИМолчитПроСвою) {
    const fs::path base = FreshDir("watch");
    Project project;
    std::string error;
    CHECK_TRUE(project.Create(base, "Игра", error));
    WriteScene(project, "main.sage");

    sage::project::ProjectWatcher watcher;
    watcher.SetInterval(0.0);   // в проверке ждать секунду незачем
    watcher.Watch(project);
    CHECK_TRUE(watcher.Watching());

    // Сразу после начала слежения изменений нет: первый снимок — не событие,
    // иначе открытие проекта сообщало бы о сотне «новых» файлов.
    CHECK_TRUE(watcher.Poll(1.0).empty());

    // Правка СНАРУЖИ (скрипт, второй редактор, генератор уровней) — замечена.
    // Размер меняем заодно с содержимым: время записи у некоторых файловых
    // систем огрубляется до секунды, и правка внутри той же секунды по нему не
    // видна — на этом проверка и ловила бы себя сама.
    {
        std::ofstream out(project.ScenesDir() / "main.sage");
        out << R"({"name":"Правка снаружи","sage_scene_version":5,"objects":[],"note":"извне"})";
    }
    const std::vector<sage::project::Change> changes = watcher.Poll(2.0);
    CHECK_EQ(changes.size(), (size_t)1);
    CHECK_EQ(changes[0].Path, std::string("scenes/main.sage"));
    CHECK_TRUE(changes[0].Kind == sage::project::ChangeKind::Modified);

    // Новый файл — тоже событие.
    WriteScene(project, "level2.sage");
    const std::vector<sage::project::Change> added = watcher.Poll(3.0);
    CHECK_EQ(added.size(), (size_t)1);
    CHECK_TRUE(added[0].Kind == sage::project::ChangeKind::Added);

    // А вот СВОЯ запись событием быть не должна. Без этого каждое «Сохранить»
    // в редакторе возвращалось бы вопросом «файл изменён снаружи,
    // перезагрузить?» — и человек перестал бы читать этот вопрос вообще,
    // включая тот раз, когда он настоящий.
    {
        std::ofstream out(project.ScenesDir() / "main.sage");
        out << R"({"name":"Своя запись","sage_scene_version":5,"objects":[],"n":"своё"})";
    }
    watcher.MarkOwnWrite(project.ScenesDir() / "main.sage");
    CHECK_TRUE(watcher.Poll(4.0).empty());

    // Удаление замечается тоже.
    std::error_code ec;
    fs::remove(project.ScenesDir() / "level2.sage", ec);
    const std::vector<sage::project::Change> removed = watcher.Poll(5.0);
    CHECK_EQ(removed.size(), (size_t)1);
    CHECK_TRUE(removed[0].Kind == sage::project::ChangeKind::Removed);
}

TEST(Project_ВотчерНеЗамечаетЧужиеРасширения) {
    // Следим за тем, что правят осмысленно: сцены, скрипты, материалы. Логи,
    // временные файлы редактора и мусор сборки событиями быть не должны —
    // иначе вопрос «файл изменён снаружи» появлялся бы от каждого чиха.
    const fs::path base = FreshDir("watch-ext");
    Project project;
    std::string error;
    CHECK_TRUE(project.Create(base, "Игра", error));

    sage::project::ProjectWatcher watcher;
    watcher.SetInterval(0.0);
    watcher.Watch(project);
    CHECK_TRUE(watcher.Poll(1.0).empty());

    std::ofstream(project.Dir() / "сборка.log") << "шум";
    std::ofstream(project.AssetsDir() / "картинка.png") << "не текст";
    CHECK_TRUE(watcher.Poll(2.0).empty());

    // А скрипт — замечается.
    std::ofstream(project.ScriptsDir().parent_path() / "логика.lua") << "-- код";
    CHECK_EQ(watcher.Poll(3.0).size(), (size_t)1);
}

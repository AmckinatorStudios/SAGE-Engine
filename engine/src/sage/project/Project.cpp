#include "sage/project/Project.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Pack.h"
#include "sage/core/Log.h"
#include "sage/core/SaveGame.h"
#include "sage/core/Version.h"
#include "sage/scene/Prefab.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace sage::project {

namespace {

// Дополняет имя сцены расширением, если его не дописали. Имя сцены человек
// пишет и в project.sageproj, и в командной строке, и «main» там встречается
// не реже, чем «main.sage», — требовать расширение значило бы ловить человека
// на ровном месте.
std::string WithSceneExt(const std::string& name) {
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, layout::kSceneExt) == 0) return name;
    return name + layout::kSceneExt;
}

} // namespace

// ---------------------------------------------------------------------------
//  Жизненный цикл
// ---------------------------------------------------------------------------

bool Project::Create(const fs::path& baseDir, const std::string& name, std::string& error) {
    if (name.empty()) {
        error = "Имя проекта пустое";
        return false;
    }
    const fs::path dir = baseDir / name;

    std::error_code ec;
    if (fs::exists(dir) && !fs::is_empty(dir, ec)) {
        error = "Папка уже существует и не пуста: " + dir.string();
        return false;
    }
    fs::create_directories(dir / layout::kScenesDir, ec);
    fs::create_directories(dir / layout::kAssetsDir, ec);
    if (ec) {
        error = "Не удалось создать папки проекта: " + ec.message();
        return false;
    }

    m_dir = dir;
    m_info = Info{};
    m_info.Name = name;
    m_info.EngineVersion = kSageEngineVersion;
    m_loaded = true;

    if (!Save(error)) {
        m_loaded = false;
        return false;
    }

    // ТО ЖЕ, что при открытии существующего проекта.
    //
    // Раньше этого здесь не было, и разница выходила не косметическая: база
    // ассетов узнавала корень проекта только в Open, а до тех пор
    // AssetDatabase::LocatePath отдавала путь как есть. То есть в ТОЛЬКО ЧТО
    // созданном проекте — самый обычный сценарий «создал проект, кладу свои
    // модели» — ссылка вида «assets/models/герой.obj» искалась от рабочего
    // каталога программы, файла там не было, и ассет не грузился. Лечилось
    // случайно: закрыть и открыть проект заново.
    Adopt();
    LOG_INFO("Project") << "Проект создан: " << dir.string();
    return true;
}

bool Project::Open(const fs::path& fileOrDir, std::string& error) {
    std::error_code ec;
    fs::path file = fileOrDir;
    if (fs::is_directory(file, ec)) file /= layout::kProjectFile;
    if (!fs::exists(file, ec)) {
        error = "Файл проекта не найден: " + file.string();
        return false;
    }

    json root;
    try {
        std::ifstream in(file);
        in >> root;
    } catch (const std::exception& e) {
        error = std::string("Файл проекта не разбирается: ") + e.what();
        return false;
    }


    m_dir = file.parent_path();
    m_info = Info{};
    // Имя по умолчанию — имя папки: проект без поля name открыть можно, и это
    // лучше, чем отказ. Безымянный проект бывает у того, кто создал папку
    // руками, и терять из-за этого работу незачем.
    m_info.Name = root.value("name", m_dir.filename().string());
    m_info.FormatVersion = root.value("sage_project_version", 1);
    m_info.StartScene = root.value("start_scene", std::string{});
    m_info.EngineVersion = root.value("engine_version", std::string{});
    m_loaded = true;

    // Стандартные подпапки, база ассетов, имя для сохранений, кэш префабов —
    // всё то же, что и при создании проекта (см. Adopt).
    Adopt();

    LOG_INFO("Project") << "Проект открыт: " << m_info.Name << " (" << m_dir.string() << ")";
    return true;
}

bool Project::Save(std::string& error) const {
    if (!m_loaded) {
        error = "Проект не открыт";
        return false;
    }
    json root;
    root["sage_project_version"] = m_info.FormatVersion;
    root["name"] = m_info.Name;
    // Пустые поля НЕ пишем: файл проекта читает и правит человек, и строка
    // "start_scene": "" сообщает ему ровно ничего, зато выглядит как настройка,
    // которую он якобы уже задал.
    if (!m_info.StartScene.empty()) root["start_scene"] = m_info.StartScene;
    if (!m_info.EngineVersion.empty()) root["engine_version"] = m_info.EngineVersion;

    std::ofstream file(ProjectFile());
    if (!file.is_open()) {
        error = "Не удалось записать файл проекта: " + ProjectFile().string();
        return false;
    }
    file << root.dump(2) << "\n";
    return true;
}

void Project::Close() {
    m_loaded = false;
    m_info = Info{};
    m_dir.clear();
}

void Project::AdoptDirectory(const fs::path& dir, const std::string& name) {
    m_dir = dir;
    m_info = Info{};
    m_info.Name = name.empty() ? dir.filename().string() : name;
    m_loaded = true;
    Adopt();
    LOG_INFO("Project") << "Проект принят без файла описания: " << m_info.Name;
}

void Project::Adopt() {
    std::error_code ec;
    fs::create_directories(ScenesDir(), ec);
    fs::create_directories(AssetsDir(), ec);

    // База ассетов: сканируется ДО загрузки сцены, иначе сцена спрашивала бы
    // про GUID'ы у пустой базы и каждая ссылка выглядела бы сломанной.
    // Смена проекта сбрасывает базу целиком: записи одного проекта в другом
    // означают ответы про файлы, которых там нет.
    sage::AssetDatabase::Instance().Clear();
    sage::AssetDatabase::Instance().ScanProject(m_dir.string());

    // Сохранения идут в папку с именем проекта — одинаково в редакторе и в
    // собранной игре. Иначе проверить работу сохранений в редакторе было бы
    // нечем: он писал бы в одно место, а игра читала из другого.
    sage::save::SetGameName(m_info.Name);
    sage::scene::ClearPrefabCache();   // префабы прошлого проекта тут ни при чём
}

// ---------------------------------------------------------------------------
//  Сцены
// ---------------------------------------------------------------------------

std::vector<std::string> Project::SceneNames() const {
    std::vector<std::string> names;

    // Пакет старше диска: у собранной игры каталога scenes/ на диске нет
    // вовсе. Проверяем именно Mounted(), а не «нашлось ли что-то»: пустой
    // смонтированный пакет и отсутствие пакета — разные состояния, и во втором
    // случае надо честно посмотреть на диск.
    if (sage::assets::vfs::Mounted()) {
        for (const std::string& path :
             sage::assets::vfs::ListFiles(layout::kScenesDir, layout::kSceneExt)) {
            names.push_back(fs::path(path).filename().string());
        }
    } else if (m_loaded) {
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::directory_iterator(ScenesDir(), ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != layout::kSceneExt) continue;
            names.push_back(entry.path().filename().string());
        }
    }

    // Порядок обхода каталога системой не определён — сортируем, иначе «первая
    // сцена» означала бы разное на разных машинах, а с ней и то, какая сцена
    // запустится у игрока.
    std::sort(names.begin(), names.end());
    return names;
}

fs::path Project::ScenePath(const std::string& name) const {
    if (name.empty()) return {};
    return ScenesDir() / WithSceneExt(name);
}

std::string Project::SceneRef(const std::string& name) const {
    if (name.empty()) return {};
    return std::string(layout::kScenesDir) + "/" + WithSceneExt(name);
}

std::string Project::StartSceneName() const {
    const std::vector<std::string> scenes = SceneNames();
    if (scenes.empty()) return {};

    auto has = [&](const std::string& file) {
        return std::find(scenes.begin(), scenes.end(), file) != scenes.end();
    };

    // 1. Явный выбор автора игры. Он старше любых соглашений — ради этого поле
    // и заведено.
    if (!m_info.StartScene.empty()) {
        const std::string wanted = WithSceneExt(m_info.StartScene);
        if (has(wanted)) return wanted;
        // Указана, но её нет. Молча подставить другую значило бы запустить не
        // ту игру и не сказать об этом.
        LOG_WARN("Project") << "стартовая сцена «" << m_info.StartScene
                            << "» в проекте не найдена — беру сцену по умолчанию";
    }

    // 2. Соглашение.
    if (has(layout::kDefaultScene)) return layout::kDefaultScene;

    // 3. Первая по алфавиту — чтобы проект без main.sage всё-таки запускался.
    return scenes.front();
}

fs::path Project::StartScenePath() const {
    const std::string name = StartSceneName();
    return name.empty() ? fs::path{} : ScenesDir() / name;
}

std::string Project::StartSceneRef() const {
    return SceneRef(StartSceneName());
}

// ---------------------------------------------------------------------------
//  Ассеты и поиск
// ---------------------------------------------------------------------------

std::string Project::AssetRef(const fs::path& path) const {
    if (path.empty()) return {};
    if (!m_loaded) return path.generic_string();

    std::error_code ec;
    // weakly_canonical, а не relative по строкам: путь мог прийти с «..», с
    // символической ссылкой или просто в другом регистре диска, и сравнение
    // строк объявило бы файл проекта чужим.
    const fs::path full = fs::weakly_canonical(path, ec);
    const fs::path root = fs::weakly_canonical(m_dir, ec);
    if (ec) return path.generic_string();

    const fs::path rel = fs::relative(full, root, ec);
    if (ec || rel.empty()) return path.generic_string();
    const std::string text = rel.generic_string();
    // «..» в начале — файл лежит выше проекта, то есть вне его.
    if (text.rfind("..", 0) == 0) return path.generic_string();
    return text;
}

bool Project::LooksLikeProject(const fs::path& fileOrDir) {
    std::error_code ec;
    if (fileOrDir.empty()) return false;
    if (fs::is_directory(fileOrDir, ec)) {
        return fs::exists(fileOrDir / layout::kProjectFile, ec);
    }
    return fileOrDir.filename() == layout::kProjectFile && fs::exists(fileOrDir, ec);
}

fs::path Project::Find(const fs::path& start) {
    std::error_code ec;
    fs::path dir = start.empty() ? fs::current_path(ec) : start;
    if (fs::is_regular_file(dir, ec)) dir = dir.parent_path();
    dir = fs::absolute(dir, ec);
    if (ec) return {};

    // Вверх до корня файловой системы. Ограничения по глубине нет намеренно:
    // проект лежит там, где его положил человек, а не там, где нам удобно.
    // Выход обеспечивает сам путь — parent_path корня равен корню.
    for (fs::path cur = dir;; cur = cur.parent_path()) {
        const fs::path candidate = cur / layout::kProjectFile;
        if (fs::exists(candidate, ec)) return candidate;
        if (cur == cur.parent_path()) break;
    }
    return {};
}

} // namespace sage::project

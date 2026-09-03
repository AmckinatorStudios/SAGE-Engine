#include "Project.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include "sage/core/Log.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/SaveGame.h"
#include "sage/scene/Prefab.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

bool Project::CreateNew(const fs::path& baseDir, const std::string& name, std::string& error) {
    if (name.empty()) { error = "Project name is empty"; return false; }
    fs::path dir = baseDir / name;

    std::error_code ec;
    if (fs::exists(dir) && !fs::is_empty(dir, ec)) {
        error = "Directory already exists and is not empty: " + dir.string();
        return false;
    }
    fs::create_directories(dir / "scenes", ec);
    fs::create_directories(dir / "assets", ec);
    if (ec) { error = "Failed to create project directories: " + ec.message(); return false; }

    json j;
    j["sage_project_version"] = 1;
    j["name"] = name;
    std::ofstream file(dir / "project.sageproj");
    if (!file.is_open()) { error = "Failed to write project file"; return false; }
    file << j.dump(2);

    m_dir = dir;
    m_name = name;
    m_loaded = true;
    // ТО ЖЕ, что при открытии существующего проекта.
    //
    // Раньше этого здесь не было, и разница выходила не косметическая: база
    // ассетов узнавала корень проекта только в Open, а до тех пор
    // AssetDatabase::LocatePath отдавала путь как есть. То есть в ТОЛЬКО ЧТО
    // созданном проекте — самый обычный сценарий «File > New Project, кладу
    // свои модели» — ссылка вида «assets/models/герой.obj» искалась от рабочего
    // каталога РЕДАКТОРА, файла там не было, и ассет не грузился. Лечилось
    // случайно: закрыть и открыть проект заново.
    Adopt();
    LOG_INFO("Editor") << "Project created: " << dir.string();
    return true;
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

    // Сохранения Play-режима идут в ту же папку, что и у собранной игры: иначе
    // проверить работу сохранений в редакторе было бы нечем — она писала бы в
    // одно место, а игра читала из другого.
    sage::save::SetGameName(m_name);
    sage::scene::ClearPrefabCache();   // префабы прошлого проекта тут ни при чём
}

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

bool Project::Open(const fs::path& fileOrDir, std::string& error) {
    fs::path file = fileOrDir;
    if (fs::is_directory(file)) file /= "project.sageproj";
    if (!fs::exists(file)) { error = "Project file not found: " + file.string(); return false; }

    std::ifstream in(file);
    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        error = std::string("Invalid project file: ") + e.what();
        return false;
    }

    m_dir = file.parent_path();
    m_name = j.value("name", m_dir.filename().string());
    m_loaded = true;

    // Стандартные подпапки, база ассетов, имя для сохранений, кэш префабов —
    // всё то же, что и при создании проекта (см. Adopt).
    Adopt();

    LOG_INFO("Editor") << "Project opened: " << m_name << " (" << m_dir.string() << ")";
    return true;
}

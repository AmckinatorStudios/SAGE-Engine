#pragma once
#include <filesystem>
#include <string>

// ---------------------------------------------------------------------------
// Project — проект игры в редакторе. Проект = папка с файлом project.sageproj
// (JSON: имя, версия формата) и стандартными подпапками:
//   <dir>/project.sageproj
//   <dir>/scenes/   — файлы сцен .sage
//   <dir>/assets/   — ассеты игры (модели, текстуры, скрипты, звук)
// Редактор создаёт проекты (File > New Project), открывает существующие и
// сохраняет/грузит сцены внутрь scenes/. Игры на движке остаются независимыми:
// проект — это сущность редактора, движку о нём знать не нужно.
// ---------------------------------------------------------------------------
class Project {
public:
    bool Loaded() const { return m_loaded; }
    const std::string& Name() const { return m_name; }
    const std::filesystem::path& Dir() const { return m_dir; }
    std::filesystem::path ScenesDir() const { return m_dir / "scenes"; }
    std::filesystem::path AssetsDir() const { return m_dir / "assets"; }
    std::filesystem::path ProjectFile() const { return m_dir / "project.sageproj"; }

    // Создаёт <baseDir>/<name>/ с project.sageproj, scenes/ и assets/ и делает
    // проект текущим. false + error, если папка занята или запись не удалась.
    bool CreateNew(const std::filesystem::path& baseDir, const std::string& name, std::string& error);

    // Открывает существующий проект: путь к project.sageproj или к папке с ним.
    bool Open(const std::filesystem::path& fileOrDir, std::string& error);

    void Close() { m_loaded = false; m_name.clear(); m_dir.clear(); }

private:
    bool m_loaded = false;
    std::string m_name;
    std::filesystem::path m_dir;
};

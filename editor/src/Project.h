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

    // Путь ассета в том виде, в каком его надо ЗАПИСАТЬ в сцену, материал или
    // компонент: относительно корня проекта, через '/'.
    //
    // ЗАЧЕМ. Панель Assets и файловые диалоги отдают АБСОЛЮТНЫЙ путь
    // («/home/вася/Проекты/Игра/assets/models/лодка.obj»), и до сих пор он в
    // таком виде и уезжал в сцену. В редакторе на этой машине всё работало —
    // и ровно там заканчивалось. Собранная игра делает chdir в свою папку и
    // ищет «assets/models/лодка.obj»; абсолютного пути с чужой машины у неё
    // нет, поэтому модель молча не грузилась, а сцена показывала пустоту.
    // Тот же результат давали перенос проекта в другую папку, второй
    // компьютер и любой архив, отданный товарищу.
    //
    // Путь ВНЕ проекта возвращается как есть: сослаться на файл в чужой папке
    // — законное временное состояние (человек только что выбрал модель в
    // «Загрузках»), и подменять его выдуманным относительным было бы враньём.
    // Такой ассет становится переносимым, когда его вносят в проект (Импорт в
    // панели Assets).
    std::string AssetRef(const std::filesystem::path& path) const;

    void Close() { m_loaded = false; m_name.clear(); m_dir.clear(); }

private:
    // Сделать m_dir/m_name ДЕЙСТВУЮЩИМ проектом: подпапки, база ассетов, имя
    // для сохранений, сброс кэша префабов. Общее для CreateNew и Open — и это
    // не «вынес дубликат»: раньше всё это делал только Open, и созданный
    // проект оставался наполовину подключённым (см. комментарий в CreateNew).
    void Adopt();

    bool m_loaded = false;
    std::string m_name;
    std::filesystem::path m_dir;
};

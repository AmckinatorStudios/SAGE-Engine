#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// СБОРКА ИГРЫ В ФОНЕ.
//
// Сборка — это упаковка тысяч файлов с сжатием и шифрованием: на настоящем
// проекте это десятки секунд. Пока она шла в главном потоке, редактор висел
// белым окном без единого признака жизни, и выглядело это как зависание
// (именно так о нём и сообщили). Здесь она идёт в своём потоке, а окно сборки
// каждый кадр спрашивает Progress()/Stage() и рисует полосу.
//
// Что попадает в игру (одним пакетом game.sagepak рядом с exe):
//   • ВЕСЬ проект — сцены, скрипты, модели, текстуры, звук, материалы,
//     сайдкары импорта (.sageimport — без них модель в игре собиралась бы с
//     другим масштабом, чем в редакторе); не едут только .meta и служебные
//     папки на точку;
//   • оглавление базы ассетов (GUID -> путь), иначе ссылки сцены по GUID
//     в игре не находились бы;
//   • ресурсы движка (шейдеры, шрифт) — россыпи assets/ рядом с exe больше нет.
// Рядом с пакетом — только исполняемый файл, его библиотеки (.dll) и sage.cfg
// (его игрок вправе править сам).
// ---------------------------------------------------------------------------
class GameBuilder {
public:
    struct Request {
        std::filesystem::path ProjectDir;
        std::string ProjectName;
        std::filesystem::path OutputDir;      // игра ляжет в OutputDir/ProjectName
        std::filesystem::path Player;         // найденный SagePlayer
        std::string AssetIndex;               // AssetDatabase::ExportIndex()
        bool CopyProjectFile = false;         // см. EngineConfig::BuildProjectFile
    };

    ~GameBuilder();

    // Запустить сборку. false — уже идёт.
    bool Start(const Request& request);
    // Сборка целиком в вызывающем потоке (самопроверка, командная строка).
    static bool RunSync(const Request& request, std::string& err);

    bool Running() const { return m_running.load(); }
    // 0..1 — доля сделанного.
    float Progress() const { return m_progress.load(); }
    std::string Stage() const;
    void Cancel() { m_cancel = true; }

    // Закончилась ли последняя сборка и чем. Сбрасывается Start().
    bool Finished() const { return m_finished.load(); }
    bool Succeeded() const { return m_ok.load(); }
    std::string Error() const;
    std::filesystem::path GameDir() const;

private:
    struct Shared;
    static bool Run(const Request& request, Shared& shared);
    void Join();

    std::thread m_thread;
    std::atomic<bool> m_running{false}, m_finished{false}, m_ok{false}, m_cancel{false};
    std::atomic<float> m_progress{0.0f};
    mutable std::mutex m_mx;
    std::string m_stage, m_error;
    std::filesystem::path m_gameDir;
};

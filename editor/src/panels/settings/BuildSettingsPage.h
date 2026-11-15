#pragma once
#include <filesystem>
#include <string>

#include "AssetPreview.h"
#include "AssetSlot.h"
#include "FileBrowser.h"
#include "sage/assets/GameInfo.h"

class EditorHost;

// ---------------------------------------------------------------------------
// СТРАНИЦА «BUILD SETTINGS» окна «Настройки» — чем собранная игра
// представляется: название (окно и имя exe), описание, автор, издатель,
// версия, копирайт, иконка, папки для файлов игрока. Хранятся в
// project.sageproj (раздел "game", sage/assets/GameInfo.h); их читают сборка
// (иконка и сведения о файле в .exe) и плеер (заголовок и иконка окна).
//
// Своего окна нет: DrawBody рисует только содержимое страницы, окно — у
// SettingsPanel. Изменения пишутся в файл проекта сразу, по завершении правки
// поля, — мимо «Применить / Отмена / По умолчанию» окна настроек: те про
// sage.cfg, и сбросить ими сведения об игре было бы потерей данных.
// ---------------------------------------------------------------------------
class BuildSettingsPage {
public:
    void DrawBody(EditorHost& host);
    // Итог последней записи (для низа окна настроек); пусто — ещё не писали.
    const std::string& Status() const { return m_status; }
    bool StatusError() const { return m_statusError; }

private:
    void Load(const std::filesystem::path& dir);
    void Save();
    bool DrawUserFolders();

    std::filesystem::path m_dir;          // проект, чьи настройки открыты
    sage::assets::GameInfo m_info;
    std::string m_status;
    bool m_statusError = false;
    AssetPreview m_preview;
    FileBrowser m_browser;
};

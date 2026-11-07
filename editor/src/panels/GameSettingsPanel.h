#pragma once
#include <filesystem>
#include <string>

#include "AssetPreview.h"
#include "AssetSlot.h"
#include "FileBrowser.h"
#include "sage/assets/GameInfo.h"

class EditorHost;

// ---------------------------------------------------------------------------
// «НАСТРОЙКИ ИГРЫ» — чем собранная игра представляется: название (окно и имя
// exe), описание, автор, издатель, версия, копирайт, иконка. Хранятся в
// project.sageproj (раздел "game", sage/assets/GameInfo.h); их читают сборка
// (иконка и сведения о файле в .exe) и плеер (заголовок и иконка окна).
// Изменения пишутся в файл проекта сразу, по завершении правки поля.
// ---------------------------------------------------------------------------
class GameSettingsPanel {
public:
    void Draw(EditorHost& host, bool* open);

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

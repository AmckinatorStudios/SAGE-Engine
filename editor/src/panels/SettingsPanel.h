#pragma once

class EditorHost;

// Панель Settings — окно гибкой конфигурации игры (EngineConfig, см.
// host.Settings()): окно/дисплей/графика/пост-эффекты. Правки сохраняются в
// <проект>/sage.cfg (Build Game кладёт файл в собранную игру; SagePlayer/игра
// читают его при запуске). Оконные параметры применяются при следующем запуске
// игры, не в самом редакторе. Видимостью управляет вызывающий (флаг open —
// меню Window > Settings и крестик окна пишут в него).
class SettingsPanel {
public:
    void Draw(EditorHost& host, bool& open);
};

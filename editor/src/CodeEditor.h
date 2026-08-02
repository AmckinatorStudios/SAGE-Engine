#pragma once
#include <filesystem>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Редактор кода со подсветкой синтаксиса.
//
// ЗАЧЕМ ОН В РЕДАКТОРЕ ДВИЖКА. Игра на SAGE — это Lua-скрипты и GLSL-шейдеры, и
// оба правятся постоянно: логика, параметры материала, эффект воды. До этого
// редактор умел их только ЗАПУСКАТЬ — править приходилось во внешнем редакторе,
// переключаясь туда-обратно на каждую строчку. Хуже другое: движок умеет
// перечитывать скрипты и шейдеры на лету, и вся ценность этого терялась, потому
// что «на лету» упиралось в alt-tab.
//
// ПОЧЕМУ НЕ InputTextMultiline. Он не умеет ни подсветки, ни номеров строк, ни
// перехода к строке с ошибкой — то есть ровно того, ради чего редактор кода и
// открывают. Поэтому текст рисуется вручную через ImDrawList: это не «свой
// текстовый движок», а разбор строки на цветные куски плюс курсор.
//
// ЧЕГО ЗДЕСЬ НЕТ И НЕ БУДЕТ: автодополнения по типам, рефакторингов, отладчика.
// Это работа языкового сервера, а не редактора сцены. Здесь — то, без чего
// правка скрипта превращается в мучение: подсветка, номера строк, поиск,
// переход к строке, отмена, сохранение по Ctrl+S и переход к строке с ошибкой
// прямо из консоли.
// ---------------------------------------------------------------------------
class CodeEditor {
public:
    enum class Syntax { PlainText, Lua, Glsl };

    // Открывает файл во вкладке (или переключается на уже открытую).
    // false — файл не читается.
    bool OpenFile(const std::filesystem::path& path);

    // Ставит курсор на строку (1-based) в файле — для перехода к месту ошибки.
    void GoToLine(const std::filesystem::path& path, int line);

    // Рисует окно. open — флаг видимости из меню Window.
    void Draw(bool* open);

    bool HasUnsaved() const;
    // Сохраняет все изменённые вкладки. Возвращает число сохранённых.
    int SaveAll();

private:
    struct UndoStep {
        std::vector<std::string> Lines;
        int CursorLine = 0, CursorCol = 0;
    };

    struct Tab {
        std::filesystem::path Path;
        std::vector<std::string> Lines{""};
        Syntax Kind = Syntax::PlainText;
        bool Dirty = false;
        int CursorLine = 0, CursorCol = 0;
        int SelectLine = -1, SelectCol = -1;   // якорь выделения (-1 — нет)
        float ScrollToLine = -1.0f;            // >=0 — проскроллить к строке
        std::vector<UndoStep> Undo, Redo;
    };

    void DrawTabBar();
    void DrawToolbar(Tab& tab);
    void DrawText(Tab& tab);
    void HandleInput(Tab& tab);
    void PushUndo(Tab& tab);
    bool Save(Tab& tab);
    static Syntax DetectSyntax(const std::filesystem::path& path);

    std::vector<Tab> m_tabs;
    int m_active = -1;
    char m_find[128] = {0};
    char m_gotoLine[16] = {0};
    bool m_showFind = false;
    std::string m_status;
};

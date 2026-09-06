#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "sage/core/Log.h"
#include "ui/EditorPanel.h"

// Панель Console — живой сток лога движка. Полностью самодостаточна: сама
// регистрирует Log-sink в Attach() и снимает его в Detach() (сток ссылается
// на this — снять обязательно ДО разрушения панели).
//
// Раньше панель была списком строк без фильтров, и в этом была практическая
// беда: единственное предупреждение о непрочитанном шейдере тонуло среди
// нескольких тысяч сообщений загрузки, а именно оно и объясняло, почему сцена
// выглядит не так. Поэтому здесь фильтры по уровню, поиск, счётчики и
// сворачивание повторов — всё ради одного: чтобы ошибку было видно.
//
// ПЕРВАЯ ПАНЕЛЬ НА SAGE UI. Она собирается один раз (Build) и потом только
// обновляет строки (Sync) — см. ui/EditorPanel.h. Выбрана первой потому, что у
// неё есть всё, на чём обвязка могла бы сломаться, и ничего сверх этого: список
// переменной длины, прокрутка, галки, поле поиска и раскраска по уровню.
class ConsolePanel : public EditorPanel {
public:
    void Attach();
    void Detach();

    const char* PanelId() const override { return "console"; }
    const char* Title() const override { return "Console"; }
    void Build(sage::ui::sui::UIContext& ui, sage::ui::sui::UIElement* root) override;
    void Sync(float dt) override;

    // Сколько было предупреждений и ошибок за сессию — редактор показывает это
    // в статусной строке, чтобы не приходилось открывать панель ради проверки,
    // всё ли в порядке.
    int WarnCount() const { return m_warnCount; }
    int ErrorCount() const { return m_errorCount; }
    // Есть ли новые ошибки с прошлого взгляда (для подсветки вкладки).
    bool HasUnseenErrors() const { return m_errorCount > m_seenErrors; }
    void MarkSeen() { m_seenErrors = m_errorCount; }

    // Видимые строки одной строкой — для кнопки «Копировать» и для тестов.
    std::string VisibleText() const;
    // Сколько строк проходит фильтр прямо сейчас. Нужно самопроверке: «панель
    // показывает ровно то, что должна» иначе проверяется только глазами.
    int VisibleCount() const;

private:
    struct Entry {
        LogLevel Level;
        std::string Category;
        std::string Message;
        int Repeats = 1;   // сколько раз подряд повторилось одно и то же
    };

    bool Passes(const Entry& e) const;
    void RebuildRows();

    std::vector<Entry> m_entries;
    mutable std::mutex m_mutex;
    bool m_autoScroll = true;
    // Фильтры по уровню. Debug выключен по умолчанию: он полезен точечно, а
    // включённым всегда просто вытесняет из окна всё остальное.
    bool m_showDebug = false;
    bool m_showInfo = true;
    bool m_showWarn = true;
    bool m_showError = true;
    bool m_collapse = true;    // сворачивать одинаковые подряд идущие строки
    std::string m_filter;

    int m_warnCount = 0;
    int m_errorCount = 0;
    int m_seenErrors = 0;

    // --- Дерево панели ---
    //
    // Строки переиспользуются, а не пересоздаются: лог обновляется каждый кадр,
    // и пересборка полутора тысяч узлов на каждое сообщение — это ровно то, от
    // чего уходили, отказываясь от immediate-mode.
    sage::ui::sui::UIContext* m_ui = nullptr;
    sage::ui::sui::ScrollView* m_scroll = nullptr;
    sage::ui::sui::Label* m_empty = nullptr;
    sage::ui::sui::Checkbox* m_cbDebug = nullptr;
    sage::ui::sui::Checkbox* m_cbInfo = nullptr;
    sage::ui::sui::Checkbox* m_cbWarn = nullptr;
    sage::ui::sui::Checkbox* m_cbError = nullptr;
    sage::ui::sui::Checkbox* m_cbCollapse = nullptr;
    sage::ui::sui::Checkbox* m_cbScroll = nullptr;
    sage::ui::sui::TextInput* m_search = nullptr;
    // Подписи со счётчиками. Счётчик в подписи фильтра — не украшение: «вижу,
    // что есть три ошибки» и «хочу видеть только их» становятся одним движением
    // мышью, а без числа панель приходится открывать, чтобы просто узнать, есть
    // ли там что-нибудь.
    sage::ui::sui::Label* m_warnCaption = nullptr;
    sage::ui::sui::Label* m_errorCaption = nullptr;
    int m_shownWarn = -1;
    int m_shownError = -1;
    std::vector<sage::ui::sui::Label*> m_rows;
    // Отпечаток видимого списка: пока он не изменился, строки не трогаем.
    size_t m_shownHash = 0;
    bool m_dirtyRows = true;
};

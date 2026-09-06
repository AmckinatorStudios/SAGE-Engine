#include "ConsolePanel.h"

#include <algorithm>
#include <cstdio>
#include <functional>

#include "EditorTheme.h"
#include "ui/Clipboard.h"
#include "../Localization.h"

using namespace sage::ui;
using namespace sage::ui::sui;

namespace {

// Цвет строки по уровню. Одно место на всю панель: раскраска, размазанная по
// коду отрисовки, расходится с раскраской счётчиков — и «жёлтое» в списке
// перестаёт означать то же, что «жёлтое» в шапке.
UIColor ColorOf(LogLevel level) {
    const ImVec4 c = level >= LogLevel::Error
                         ? EditorTheme::Color(EditorTheme::Role::Danger)
                     : level == LogLevel::Warn ? EditorTheme::Color(EditorTheme::Role::Warn)
                     : level <= LogLevel::Debug
                         ? EditorTheme::Color(EditorTheme::Role::TextDim)
                         : EditorTheme::Color(EditorTheme::Role::Text);
    return UIColor(c.x, c.y, c.z, c.w);
}

std::string LineOf(const std::string& category, const std::string& message, int repeats,
                   bool collapse) {
    std::string s = "[" + category + "] " + message;
    if (collapse && repeats > 1) s += "  (x" + std::to_string(repeats) + ")";
    return s;
}

} // namespace

void ConsolePanel::Attach() {
    Log::SetSink([this](LogLevel level, const std::string& cat, const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (level == LogLevel::Warn) ++m_warnCount;
        if (level >= LogLevel::Error) ++m_errorCount;

        // Сворачивание повторов. Один кадр может выдать одно и то же
        // предупреждение тысячу раз (шейдер не собрался — и так на каждый
        // объект), и без свёртки такая строка вытесняет из окна всё остальное,
        // включая причину.
        if (!m_entries.empty()) {
            Entry& last = m_entries.back();
            if (last.Level == level && last.Category == cat && last.Message == msg) {
                ++last.Repeats;
                m_dirtyRows = true;
                return;
            }
        }
        if (m_entries.size() > 4000) m_entries.erase(m_entries.begin(), m_entries.begin() + 1000);
        m_entries.push_back({level, cat, msg, 1});
        m_dirtyRows = true;
    });
}

void ConsolePanel::Detach() { Log::SetSink(nullptr); }

bool ConsolePanel::Passes(const Entry& e) const {
    if (e.Level >= LogLevel::Error) { if (!m_showError) return false; }
    else if (e.Level == LogLevel::Warn) { if (!m_showWarn) return false; }
    else if (e.Level == LogLevel::Info) { if (!m_showInfo) return false; }
    else if (!m_showDebug) return false;

    if (!m_filter.empty()) {
        // Ищем и по категории, и по тексту: искать «Shader» и не найти
        // сообщение категории Shader было бы неожиданностью.
        if (e.Message.find(m_filter) == std::string::npos &&
            e.Category.find(m_filter) == std::string::npos) {
            return false;
        }
    }
    return true;
}

int ConsolePanel::VisibleCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    int n = 0;
    for (const Entry& e : m_entries)
        if (Passes(e)) ++n;
    return n;
}

std::string ConsolePanel::VisibleText() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string all;
    for (const Entry& e : m_entries) {
        if (!Passes(e)) continue;
        all += LineOf(e.Category, e.Message, e.Repeats, m_collapse);
        all += "\n";
    }
    return all;
}

// --- Сборка ------------------------------------------------------------------

void ConsolePanel::Build(UIContext& ui, UIElement* root) {
    m_ui = &ui;
    root->Vertical(6.0f)->Padding(UIEdges::Uniform(8.0f));

    // --- Шапка: действия, фильтры, поиск ---
    UIElement* bar = ui.CreateIn<UIElement>(root);
    bar->SetName("Toolbar");
    bar->SetHeight(26.0f);
    bar->SetStretch(true, false);
    bar->Horizontal(6.0f)->Padding(UIEdges::Uniform(0.0f));
    bar->Layout().Cross = UIAlign::Center;

    ui.CreateIn<Button>(bar, T("Clear"), std::string())
        ->FitToText()
        ->OnClick([this] {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries.clear();
            m_dirtyRows = true;
        });
    ui.CreateIn<Button>(bar, T("Copy"), std::string())
        ->FitToText()
        ->OnClick([this] { Clipboard::SetText(VisibleText()); });

    // Счётчики совмещены с переключателями уровня намеренно: «вижу, что есть
    // три ошибки» и «хочу видеть только их» — одно и то же движение мышью.
    auto flag = [&](const char* text, bool value, bool* target, const UIColor* color) {
        Checkbox* box = ui.CreateIn<Checkbox>(bar, std::string(text), value);
        box->SetHeight(22.0f);
        box->Ensure<UITransform>().WidthMode = UISizeMode::Content;
        if (color && !box->Children().empty())
            static_cast<Label*>(box->Children()[0])->SetColor(*color);
        (void)0;
        box->OnToggle([this, target](bool on) {
            *target = on;
            m_dirtyRows = true;
        });
        return box;
    };
    m_cbDebug = flag(T("Debug"), m_showDebug, &m_showDebug, nullptr);
    m_cbInfo = flag(T("Info"), m_showInfo, &m_showInfo, nullptr);

    const UIColor warnColor = ColorOf(LogLevel::Warn);
    const UIColor errColor = ColorOf(LogLevel::Error);
    char warnLabel[48], errLabel[48];
    std::snprintf(warnLabel, sizeof(warnLabel), T("Warn (%d)"), m_warnCount);
    std::snprintf(errLabel, sizeof(errLabel), T("Error (%d)"), m_errorCount);
    m_cbWarn = flag(warnLabel, m_showWarn, &m_showWarn, &warnColor);
    m_cbError = flag(errLabel, m_showError, &m_showError, &errColor);
    if (!m_cbWarn->Children().empty())
        m_warnCaption = static_cast<Label*>(m_cbWarn->Children()[0]);
    if (!m_cbError->Children().empty())
        m_errorCaption = static_cast<Label*>(m_cbError->Children()[0]);

    m_cbCollapse = flag(T("Collapse"), m_collapse, &m_collapse, nullptr);
    m_cbScroll = flag(T("Auto-scroll"), m_autoScroll, &m_autoScroll, nullptr);

    UIElement* spacer = ui.CreateIn<UIElement>(bar);
    spacer->SetName("Spacer");
    spacer->SetStretch(true, false);

    m_search = ui.CreateIn<TextInput>(bar, std::string(), std::string(T("Search...")));
    m_search->SetSize({200.0f, 22.0f});
    m_search->OnChanged([this](const std::string& text) {
        m_filter = text;
        m_dirtyRows = true;
    });

    // --- Список ---
    m_scroll = ui.CreateIn<ScrollView>(root);
    m_scroll->SetName("Log");
    m_scroll->SetStretch(true, true);

    m_empty = ui.CreateIn<Label>(m_scroll->Content(), std::string());
    m_empty->SetColor(ColorOf(LogLevel::Debug));
}

// --- Обновление --------------------------------------------------------------

void ConsolePanel::RebuildRows() {
    if (!m_ui || !m_scroll) return;

    std::vector<const Entry*> shown;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        shown.reserve(m_entries.size());
        for (const Entry& e : m_entries)
            if (Passes(e)) shown.push_back(&e);
    }

    // Строк ровно столько, сколько видно. Лишние ПРЯЧУТСЯ, а не удаляются:
    // лог скачет туда-сюда от каждого сообщения, и создавать-удалять узлы на
    // каждое движение — это тот же расход, от которого уходили.
    while (m_rows.size() < shown.size()) {
        Label* row = m_ui->CreateIn<Label>(m_scroll->Content(), std::string());
        row->SetAlign(UITextAlign::Left, UITextVAlign::Top);
        row->SetStretch(true, false);
        row->SetEllipsis(true);
        m_rows.push_back(row);
    }
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const bool used = i < shown.size();
        m_rows[i]->SetVisible(used);
        if (!used) continue;
        const Entry& e = *shown[i];
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rows[i]->SetText(LineOf(e.Category, e.Message, e.Repeats, m_collapse));
        m_rows[i]->SetColor(ColorOf(e.Level));
    }

    const bool none = shown.empty();
    m_empty->SetVisible(none);
    if (none) {
        m_empty->SetText(m_filter.empty() ? T("No messages of the selected level")
                                          : T("The filter matched nothing"));
    }
    // Пустая подсказка стоит ПОСЛЕДНЕЙ в ленте, чтобы не разрывать список:
    // порядок детей — это порядок в столбце.
    if (UINode* n = m_empty->Node()) n->Order = 1;
}

void ConsolePanel::Sync(float dt) {
    (void)dt;

    // Счётчики обновляются, только когда изменились: перезапись текста —
    // это пересчёт раскладки строки, а числа стоят на месте почти всегда.
    if (m_warnCaption && m_warnCount != m_shownWarn) {
        m_shownWarn = m_warnCount;
        char label[48];
        std::snprintf(label, sizeof(label), T("Warn (%d)"), m_warnCount);
        m_warnCaption->SetText(label);
    }
    if (m_errorCaption && m_errorCount != m_shownError) {
        m_shownError = m_errorCount;
        char label[48];
        std::snprintf(label, sizeof(label), T("Error (%d)"), m_errorCount);
        m_errorCaption->SetText(label);
    }

    if (!m_dirtyRows) return;
    m_dirtyRows = false;
    RebuildRows();

    // Автопрокрутка вниз: новое сообщение должно быть видно, не трогая мышь.
    if (m_autoScroll && m_scroll) {
        if (UIScrollView* sv = m_scroll->Get<UIScrollView>()) {
            const UIRect view = m_scroll->Bounds();
            sv->Offset.y = std::max(0.0f, sv->ContentSize.y - view.h);
        }
    }
}

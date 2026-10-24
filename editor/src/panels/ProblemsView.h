#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "../ProblemGroups.h"
#include "sage/core/Problems.h"

class EditorHost;

// ---------------------------------------------------------------------------
// ВКЛАДКА «ПРОБЛЕМЫ» КОНСОЛИ.
//
// Журнал отвечает на вопрос «что происходило», вкладка — «что сломано и как
// это починить». Строка журнала — это след; проблема — это задача: у неё есть
// место (файл и строка, открываются щелчком), объяснение, шаги исправления и
// пример, а связанные беды собраны в группу под своей первопричиной — чинить
// начинают с неё, и последствия обычно уходят вместе с ней.
//
// Снимок реестра берётся только когда он сменился (Problems::Revision): сто
// проблем каждый кадр копировать незачем.
// ---------------------------------------------------------------------------
class ProblemsView {
public:
    // host — открыть файл в редакторе кода и собрать контекст отчёта; без
    // него (тест, инструмент) вкладка просто не предлагает этих кнопок.
    void Draw(EditorHost* host);
    // Ошибок и аварий среди первопричин — для подписи вкладки.
    int OpenErrors();

private:
    void Refresh();
    void DrawToolbar(EditorHost* host);
    void DrawList();
    void DrawDetails(EditorHost* host, const sage::Problem& p, const sage::editor::problems::Group* group);
    void OpenFile(EditorHost* host, const sage::Problem& p);
    std::string ReportText(EditorHost* host);

    uint64_t m_revision = 0;
    std::vector<sage::Problem> m_snapshot;
    std::vector<sage::editor::problems::Group> m_groups;
    sage::editor::problems::Filter m_filter;
    char m_search[128] = {0};
    std::string m_appliedSearch;
    uint64_t m_selected = 0;          // номер выбранной проблемы (0 — нет)
    std::set<uint64_t> m_expanded;    // раскрытые группы
    std::string m_notice;             // «Отчёт скопирован» — на пару секунд
    double m_noticeUntil = 0.0;
};

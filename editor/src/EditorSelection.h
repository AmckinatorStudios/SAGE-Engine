#pragma once
#include <filesystem>
#include <vector>

// ---------------------------------------------------------------------------
// EditorSelection — ЧТО ВЫДЕЛЕНО в редакторе и что показывает инспектор.
//
// Выделение жило полями EditorLayer вперемешку с проектом, Play-режимом,
// панелями и плагинами. Правила у него при этом свои и совсем не очевидные:
// «первичная» — последняя добавленная (под неё встаёт инспектор и пивот
// гизмо), повторное добавление того же номера не удваивает набор, а пустой
// набор означает «ничего не выбрано», а не «первичная осталась прежней».
// Раньше эти правила можно было проверить только запустив редактор целиком;
// здесь они — отдельный класс без сцены, ImGui и графики, который проверяется
// обычным модульным тестом (tests/test_editorstate.cpp).
//
// Хранятся НОМЕРА сущностей, а не GameObject: между выделением и обращением
// сцену могли перезагрузить (откат, открытие другой), и объект по указателю
// оказался бы чужим.
// ---------------------------------------------------------------------------
class EditorSelection {
public:
    // «Первичная» — та, под которую встаёт инспектор и пивот гизмо. -1 — пусто.
    int Primary() const { return m_primary; }
    const std::vector<int>& All() const { return m_items; }
    bool Empty() const { return m_items.empty(); }
    bool Contains(int id) const;

    // Одиночное выделение: набор заменяется одним номером (-1 — очистить).
    void SetPrimary(int id);

    // Набор целиком. additive — добавить к уже выделенному, а не заменить.
    void Set(const std::vector<int>& ids, bool additive = false);

    // Щелчок с Ctrl: был в наборе — убрать, не был — добавить и сделать
    // первичным. -1 игнорируется: «ничего» нельзя ни добавить, ни снять.
    void Toggle(int id);

    void Clear() { m_items.clear(); m_primary = -1; }

    // --- Замок инспектора ---------------------------------------------------
    //
    // Замок запоминает ТО, ЧТО ПОКАЗАНО СЕЙЧАС, в момент запирания — и держит,
    // пока его не откроют. Так можно выбрать другой объект, не потеряв из виду
    // свойства прежнего.
    bool Locked() const { return m_locked; }
    void Lock(int entityId, const std::filesystem::path& assetPath);
    void Unlock() { m_locked = false; }
    int LockedEntityId() const { return m_lockedEntityId; }
    const std::filesystem::path& LockedAssetPath() const { return m_lockedAssetPath; }

private:
    std::vector<int> m_items;   // весь набор; последний — первичный
    int m_primary = -1;

    bool m_locked = false;
    int m_lockedEntityId = -1;
    std::filesystem::path m_lockedAssetPath;
};

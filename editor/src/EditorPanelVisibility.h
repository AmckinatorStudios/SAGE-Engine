#pragma once
#include "EditorTypes.h"

// ---------------------------------------------------------------------------
// EditorPanelVisibility — КАКИЕ ПАНЕЛИ СЕЙЧАС НА ЭКРАНЕ.
//
// Флаг на панель, а не «раскладка вообще». Причина не в аккуратности: у каждой
// докнутой панели ImGui рисует крестик на вкладке, и закрытая панель раньше
// исчезала НАВСЕГДА — в меню Window её не было, а раскладка сохранялась в
// sage_editor_imgui.ini, то есть закрытое окно не возвращалось и после
// перезапуска. Закрыв вкладки одну за другой, человек оставался с пустым серым
// прямоугольником и делал вывод, что «свернул весь редактор» и сломал его.
//
// Отсюда всё устройство класса: флаг на панель (закрытие обратимо), пункт в
// меню Window на тот же флаг, Restore() для «Reset Layout» и подсказки на
// пустом доке, и AnyVisible() — чтобы было чем заметить, что на экране не
// осталось ничего.
//
// МАССИВОМ ПО ПЕРЕЧИСЛЕНИЮ, а не одиннадцатью полями bool. Полей было
// одиннадцать, и каждая новая панель требовала правки в трёх местах сразу:
// поле, ветка в PanelVisible и строка в «показать все». Забытая строка в
// последней означала панель, которую нельзя вернуть, — то самое, от чего этот
// класс и защищает. С массивом забыть нечего.
// ---------------------------------------------------------------------------
class EditorPanelVisibility {
public:
    bool& operator[](EditorPanel panel) {
        const size_t i = (size_t)panel;
        // Неизвестная панель ведёт во вьюпорт: он есть всегда, и вернуть
        // ссылку на что-то надо в любом случае.
        return m_visible[i < (size_t)EditorPanel::Count ? i : (size_t)EditorPanel::Viewport];
    }
    bool operator[](EditorPanel panel) const {
        return const_cast<EditorPanelVisibility*>(this)->operator[](panel);
    }

    // Вернуть панели раскладки на экран («Reset Layout», подсказка на пустом
    // доке). Открывает ровно рабочие панели и НИЧЕГО не закрывает: «вернуть
    // панели» не должно означать «закрыть то, что человек открыл сам».
    //
    // Редактор интерфейса и профилировщик сюда не входят: это инструменты под
    // задачу, а не часть постоянной раскладки, и открывать их вместе со всем
    // остальным значит отдать место тому, кто сейчас собирает сцену.
    void Restore() {
        for (EditorPanel p : {EditorPanel::Hierarchy, EditorPanel::Inspector,
                              EditorPanel::Environment, EditorPanel::Viewport,
                              EditorPanel::Game, EditorPanel::Console, EditorPanel::Assets})
            (*this)[p] = true;
    }

    // Осталась ли на экране хоть одна панель. Если нет — на месте редактора
    // пустой серый прямоугольник, и молчать про это нельзя.
    bool AnyVisible() const {
        for (EditorPanel p : {EditorPanel::Hierarchy, EditorPanel::Inspector,
                              EditorPanel::Environment, EditorPanel::UIEditor,
                              EditorPanel::Viewport, EditorPanel::Game,
                              EditorPanel::Console, EditorPanel::Assets, EditorPanel::Profiler})
            if ((*this)[p]) return true;
        return false;
    }

private:
    // Раскладка по умолчанию: инструменты закрыты, рабочие панели открыты.
    bool m_visible[(size_t)EditorPanel::Count] = {
        true,   // Hierarchy
        true,   // Inspector
        true,   // Environment
        true,   // Assets
        true,   // Console
        false,  // Profiler — окно-инструмент
        true,   // Game
        true,   // Viewport
        false,  // UIEditor — отдельный инструмент под отдельную задачу
        false,  // Settings — окно-инструмент
        false,  // Input — окно-инструмент
    };
};

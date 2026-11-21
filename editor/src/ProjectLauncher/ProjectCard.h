#pragma once
#include <string>

#include "imgui.h"

namespace Sage::Launcher {

struct ProjectEntry;
class ProjectThumbnail;

// ---------------------------------------------------------------------------
// СТРОКА ПРОЕКТА — одна запись списка стартового окна.
//
// Строка НИЧЕГО НЕ ДЕЛАЕТ с проектом: она сообщает, что с ней случилось
// (выбрали, открыли, позвали меню), а что это значит — знает окно.
// ---------------------------------------------------------------------------

// Что случилось со строкой в этом кадре.
struct CardEvent {
    bool Selected = false;       // одиночный клик
    bool Activated = false;      // двойной клик — открыть проект
    bool MenuRequested = false;  // правая кнопка
};

// Высота строки — одно место на рисование и на виртуализацию: посчитанная
// дважды по-разному, она даёт список, который «прыгает» при прокрутке.
float RowHeight();

CardEvent DrawProjectRow(const ProjectEntry& entry, bool selected, float width,
                         ProjectThumbnail& thumbs);

// «2 часа назад», «Вчера», «3 дня назад»; дальше — дата. 0 — «не открывали».
// Строки идут через T(), числа — через формат: перевод склоняет их сам.
std::string RelativeTime(long long unixTime);

// --- контекстное меню -------------------------------------------------------

enum class ProjectAction {
    None,
    Open,
    OpenFolder,
    Rename,
    Duplicate,
    Forget,   // убрать из списка (проект на диске остаётся)
    Delete,   // удалить папку проекта с диска
};

// Пункты меню внутри уже открытого popup'а. Возвращает выбранное действие.
//
// Удаление проекта с диска стоит ОТДЕЛЬНО от «убрать из списка», через
// разделитель и красным: это единственное необратимое действие окна.
ProjectAction DrawProjectMenuItems(const ProjectEntry& entry);

} // namespace Sage::Launcher

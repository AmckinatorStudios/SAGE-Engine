#pragma once
#include <string>

#include "imgui.h"

// ---------------------------------------------------------------------------
// ЭЛЕМЕНТЫ ОКНА «НАСТРОЙКИ ИГРЫ» — один стиль на все разделы.
//
// Группа — карточка с заголовком; внутри — строки в две колонки (в узком окне —
// в одну). У строки подпись НАД полем: длинные подписи «Разрешение теней
// ламп» не режутся и не прижимаются к полю, а поля одной группы начинаются на
// одной линии. У каждой строки — как она применяется (сразу / при запуске
// игры / после перезапуска), её цена (дорогие проходы отмечены) и сброс к
// значению по умолчанию, если оно изменено.
//
// Поиск идёт по тем же строкам: Row возвращает false, если подпись не
// подходит, и группа без единой подходящей строки не рисуется вовсе.
// ---------------------------------------------------------------------------
namespace sage::editor::settingsui {

// Как изменение доходит до картинки.
enum class Apply {
    Live,      // сразу — в окне сцены и в Play
    Launch,    // при запуске игры (окно игры создаётся с этими параметрами)
    Restart,   // после перезапуска редактора/игры (бэкенд, потоки)
};

// Цена параметра: Heavy — заметно бьёт по кадру, отмечается значком.
enum class Cost { Normal, Heavy };

struct Ctx {
    std::string Filter;        // поиск в нижнем регистре; пусто — всё
    bool Narrow = false;       // одна колонка
    int Shown = 0;             // строк нарисовано (для «ничего не найдено»)
    // Группа открывается ЛЕНИВО — при первой подходящей строке.
    const char* PendingTitle = nullptr;
    const char* PendingNote = nullptr;
    bool GroupOpen = false;
    bool InTable = false;
    int GroupIndex = 0;
    int TableIndex = 0;        // таблица в группе рвётся широкой строкой
    // Как применяется ВСЯ группа: метка стоит один раз у заголовка, а строки
    // с тем же способом её не повторяют (шесть «при запуске игры» подряд —
    // шум, за которым не видно параметров).
    Apply GroupWhen = Apply::Live;

};

// Строка в нижнем регистре (латиница и кириллица) — для поиска.
std::string Lower(const char* utf8);
bool Matches(const Ctx& c, const char* label, const char* keywords);

void BeginGroup(Ctx& c, const char* title, const char* note = nullptr, Apply when = Apply::Live);
// Дальше в группе — на всю ширину карточки (пояснение, кнопка), не в колонке.
void FullWidth(Ctx& c);
// Свободный блок группы (текст, кнопки) без строки-параметра: открывает
// карточку, если её ещё нет, и участвует в поиске по label/keywords.
// false — не подходит под поиск, рисовать нечего.
bool FreeBlock(Ctx& c, const char* label, const char* keywords = nullptr);
void EndGroup(Ctx& c);

struct RowOpts {
    const char* Keywords = nullptr;   // для поиска: синонимы, английское имя
    const char* Tooltip = nullptr;    // что делает — при наведении на подпись
    Apply When = Apply::Live;
    Cost Price = Cost::Normal;
    bool Modified = false;            // отличается от значения по умолчанию
    bool Disabled = false;            // зависит от выключенной функции
    const char* DisabledWhy = nullptr;
    bool Wide = false;                // на всю ширину группы
};
// true — строка видна; тогда поле рисует вызывающий и закрывает EndRow.
// reset — нажали «сбросить» у изменённого значения.
bool Row(Ctx& c, const char* label, const RowOpts& o, bool* reset = nullptr);
void EndRow(Ctx& c, const RowOpts& o);

// Переключатель-«таблетка» (как в системных настройках): состояние читается
// издалека, в отличие от квадратика флажка.
bool Toggle(const char* id, bool* v, const char* caption = nullptr);
// Ползунок с числом рядом: ввод числа — щелчком по числу (точность).
bool SliderFloat(const char* id, float* v, float lo, float hi, const char* fmt, float step = 0.0f);
bool SliderInt(const char* id, int* v, int lo, int hi, const char* fmt);
// Строка пояснения под группой (иконка «i» + приглушённый текст).
void Note(const char* text);

} // namespace sage::editor::settingsui

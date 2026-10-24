#pragma once
#include <algorithm>
#include <string>
#include <vector>

struct ImGuiPayload;

// ---------------------------------------------------------------------------
// ПЕРЕТАСКИВАНИЕ ГРУППЫ: несколько объектов сцены или файлов проекта за раз.
//
// ЗАЧЕМ. Выделить можно было много (рамка, Ctrl, Shift), а тащилось всегда
// одно — то, за что взялись: разложить двадцать ламп по папкам значило двадцать
// раз повторить один и тот же бросок.
//
// КАК. Полезная нагрузка ImGui осталась прежней — номер объекта
// ("SAGE_ENTITY") или путь файла ("SAGE_ASSET_PATH") того, ЗА ЧТО ВЗЯЛИСЬ.
// Поэтому все цели, которым нужен один предмет (слот объекта, слот текстуры),
// работают как раньше и ничего про группы не знают. Остальная группа лежит
// здесь, рядом, и её берёт тот, кто умеет класть много (иерархия, папка,
// вьюпорт), — по той же нагрузке: если взятое не совпадает с записанным, значит
// тащат что-то другое, и группа из одного.
//
// ПОРЯДОК — тот, что на экране (сверху вниз), а не порядок щелчков: группа,
// выделенная «снизу вверх», встаёт на новое место так же, как лежала.
// ---------------------------------------------------------------------------
namespace dragset {

// Что тащить, если взялись за dragged: весь набор, если взятое в нём, иначе
// только его (взялись за невыделенное — тащат его одно, как в проводнике).
// visible — порядок строк на экране; не попавшие в него (в свёрнутой ветке)
// идут следом в порядке набора.
template <typename T>
std::vector<T> Ordered(const T& dragged, const std::vector<T>& selection, const std::vector<T>& visible) {
    if (std::find(selection.begin(), selection.end(), dragged) == selection.end()) return {dragged};
    std::vector<T> out;
    for (const T& v : visible)
        if (std::find(selection.begin(), selection.end(), v) != selection.end() &&
            std::find(out.begin(), out.end(), v) == out.end())
            out.push_back(v);
    for (const T& s : selection)
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    return out;
}

// Куда встаёт брошенное на строку списка: верхняя четверть — перед строкой,
// нижняя — после, середина — внутрь. Четверти, а не трети: «внутрь» — самый
// частый бросок, и попадать в него должно быть легче всего.
enum class Place { Before, Inside, After };
inline Place PlaceAt(float y, float top, float bottom) {
    const float h = bottom - top;
    if (h <= 0.0f) return Place::Inside;
    if (y < top + h * 0.25f) return Place::Before;
    if (y > bottom - h * 0.25f) return Place::After;
    return Place::Inside;
}

// ЩЕЛЧОК ПО ВЫДЕЛЕННОМУ В ГРУППЕ — ОДИНОЧНЫЙ ВЫБОР ПОСЛЕ ОТПУСКАНИЯ.
//
// Выбор делался на НАЖАТИИ: взялись за одну из пяти выделенных строк, чтобы
// утащить все пять, — и в тот же миг набор схлопывался до этой одной, а
// перетаскивание начиналось уже с ней. Поэтому щелчок без Ctrl/Shift по
// элементу, который уже в наборе из нескольких, откладывается: потянули —
// едет группа; отпустили, не потянув, — остаётся один (как в проводнике).
inline bool DeferSinglePick(bool alreadySelected, size_t selectionSize, bool withModifiers) {
    return alreadySelected && selectionSize > 1 && !withModifiers;
}

// Начало перетаскивания: что взяли и вся группа (взятое в ней обязано быть).
void BeginEntities(int dragged, const std::vector<int>& group);
void BeginAssets(const std::string& dragged, const std::vector<std::string>& group);

// Группа по нагрузке, которую приняла цель. Чужая или пустая нагрузка —
// группа из одного того, что в нагрузке.
std::vector<int> Entities(const ImGuiPayload* payload);
std::vector<std::string> Assets(const ImGuiPayload* payload);

// То же без ImGui — для модульных тестов и для кода, у которого нагрузка
// уже разобрана.
std::vector<int> EntitiesFor(int payloadId);
std::vector<std::string> AssetsFor(const std::string& payloadPath);

// Подпись под курсором: «Лампа» или «Лампа и ещё 4».
std::string Caption(const std::string& first, size_t count);

} // namespace dragset

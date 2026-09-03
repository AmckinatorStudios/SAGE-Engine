#pragma once
#include <string>

#include "sage/events/Events.h"
#include "sage/vars/Table.h"

class EditorHost;
class GameObject;
class AssetPreview;

// ---------------------------------------------------------------------------
// РЕДАКТОР ЗНАЧЕНИЙ: публичные переменные, ссылки и связи событий.
//
// Один набор рисовалок на все места, где встречается sage::vars::Value:
// секция «Переменные» у любого объекта, аргумент связи у кнопки, поле части
// интерфейса вида Bindings. Иначе слот ссылки с приёмом перетаскивания
// пришлось бы написать трижды, и трижды же чинить.
//
// Ссылка на объект рисуется СЛОТОМ С ПРИЁМОМ БРОСКА из дерева, а не полем для
// номера: номер сущности человек не знает и знать не должен — он видит имена.
// ---------------------------------------------------------------------------
namespace varsui {

// Одно значение под подписью. Возвращает true, если человек его изменил
// (вызывающий сам решает, когда делать PushUndoSnapshot).
bool DrawValue(EditorHost& host, const char* id, sage::vars::Value& value,
               const sage::vars::Var* meta, AssetPreview* preview);

// Слот ссылки на объект сцены: имя, приём броска из дерева, «показать», сброс.
bool DrawEntityRef(EditorHost& host, const char* id, sage::vars::EntityRef& ref);

// Секция публичных переменных объекта: список, правка, добавление, удаление.
// Возвращает true, если что-то изменилось.
bool DrawTable(EditorHost& host, GameObject obj, sage::vars::Table& table,
               AssetPreview* preview);

// Список связей «когда здесь случилось X — сделать Y».
// triggers — имена триггеров, которые умеет слать владелец списка.
bool DrawBindings(EditorHost& host, const char* id, sage::events::Bindings& bindings,
                  const std::vector<std::string>& triggers, AssetPreview* preview);

} // namespace varsui

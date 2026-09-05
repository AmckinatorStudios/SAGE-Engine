#pragma once
#include <string>
#include <vector>

#include "sage/ui/core/UIDocument.h"
#include "sage/ui/style/UIStyle.h"

// ---------------------------------------------------------------------------
// СЕРИАЛИЗАЦИЯ (§64, §65 ТЗ).
//
// ФОРМАТ ПОСТРОЕН НА ТАБЛИЦАХ СВОЙСТВ, а не написан руками. Никакого «поле за
// полем» в этом файле нет вовсе: он ходит по реестру компонентов и по их
// таблицам. Отсюда главное свойство формата — его нельзя рассинхронизировать с
// кодом: добавленное свойство сохраняется само, забыть его негде.
//
// НЕИЗВЕСТНЫЙ КОМПОНЕНТ НЕ ТЕРЯЕТСЯ. Документ, сохранённый сборкой с плагином,
// открывается сборкой без него — и при следующем сохранении чужие данные
// остаются на месте (UIUnknownComponent). Без этого один открытый файл молча
// уничтожает чужую работу.
//
// ВЕРСИЯ И ПЕРЕЕЗД (§65). В файле записана версия схемы; читатель прогоняет
// цепочку миграций до текущей. Новое свойство со значением по умолчанию не
// ломает старые файлы по определению — его просто нет в старом файле, и
// значение берётся из конструктора компонента.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Хранилище чужих данных: компонент, которого нет в этой сборке.
struct UIUnknownComponent : UIComponentOf<UIUnknownComponent> {
    static const UIComponentType& StaticType();
    std::string SourceId;
    std::string Raw; // JSON как есть

    bool SaveCustom(void* jsonObject) const override;
    bool LoadCustom(const void* jsonObject) override;
};

struct UILoadReport {
    bool Ok = false;
    int FromVersion = 0;
    int Nodes = 0;
    std::vector<std::string> Warnings; // §134: не молчать о потерях
    std::string Error;
};

// Строка JSON ← документ.
std::string UISaveDocumentToString(const UIDocument& doc, const UITheme* theme = nullptr);
// Документ ← строка JSON. Документ очищается перед чтением.
UILoadReport UILoadDocumentFromString(UIDocument& doc, const std::string& json,
                                      UITheme* theme = nullptr);

bool UISaveDocument(const UIDocument& doc, const std::string& path,
                    const UITheme* theme = nullptr);
UILoadReport UILoadDocument(UIDocument& doc, const std::string& path,
                            UITheme* theme = nullptr);

// Одно поддерево в строку и обратно — для копирования, отмены правки и
// префабов. Тот же формат, что у документа: два разных формата для «файла» и
// «буфера обмена» неизбежно разъедутся.
std::string UISaveSubtree(const UIDocument& doc, UINodeId root);
UINodeId UILoadSubtree(UIDocument& doc, UINodeId parent, const std::string& json);

// Тема отдельным файлом (§105).
std::string UISaveThemeToString(const UITheme& theme);
bool UILoadThemeFromString(UITheme& theme, const std::string& json);

// Расширение файлов документа и темы. В одном месте, потому что их знают и
// редактор, и загрузчик, и браузер ассетов.
constexpr const char* kUIDocumentExt = ".uidoc";
constexpr const char* kUIPrefabExt = ".uiprefab";
constexpr const char* kUIThemeExt = ".uitheme";

} // namespace sage::ui

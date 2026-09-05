#pragma once
#include <string>
#include <vector>

#include "sage/ui/core/UIDocument.h"

// ---------------------------------------------------------------------------
// ПРЕФАБЫ (§61–63 ТЗ) — переиспользуемые куски интерфейса.
//
// ЗАЧЕМ. Слот инвентаря, карточка, строка списка, всплывающая подсказка —
// повторяются десятками. Копия каждого — это гарантия, что через неделю они
// перестанут быть одинаковыми.
//
// КАК УСТРОЕНО. Экземпляр помнит источник (файл префаба) и СПИСОК СВОИХ
// ОТЛИЧИЙ: «у этого экземпляра свой текст и своя картинка». Обновление
// источника переносится во все экземпляры, а отличия остаются (§63). Без
// списка отличий обновление префаба стирало бы ручные правки — и им перестают
// пользоваться.
//
// ВЛОЖЕННОСТЬ (§62). Экземпляр префаба внутри другого префаба — обычный случай
// (кнопка со значком внутри карточки), и он не требует ничего особенного:
// отличия адресуются путём внутри экземпляра.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Отличие экземпляра от источника: путь свойства внутри экземпляра и значение.
struct UIPrefabOverride {
    std::string Path;   // "Title.text.Text", "Icon.image.Path"
    std::string Value;  // значение в текстовом виде (как в файле)
};

// Компонент на корне экземпляра.
struct UIPrefabInstance : UIComponentOf<UIPrefabInstance> {
    static const UIComponentType& StaticType();

    std::string Source; // путь к .uiprefab
    std::vector<UIPrefabOverride> Overrides;

    bool SaveCustom(void* jsonObject) const override;
    bool LoadCustom(const void* jsonObject) override;
};

// Создать экземпляр префаба внутри parent.
UINodeId UIInstantiatePrefab(UIDocument& doc, UINodeId parent, const std::string& path);
// Сохранить поддерево как префаб и превратить его в экземпляр.
bool UICreatePrefabFromNode(UIDocument& doc, UINodeId node, const std::string& path);
// Перечитать источник и наложить отличия заново. Возвращает число обновлённых
// экземпляров.
int UIRefreshPrefabInstances(UIDocument& doc, const std::string& sourcePath = {});
// Записать текущее состояние экземпляра в его список отличий (после ручной
// правки в редакторе).
int UICapturePrefabOverrides(UIDocument& doc, UINodeId instanceRoot);
// Разорвать связь с источником: поддерево становится обычным.
void UIUnpackPrefab(UIDocument& doc, UINodeId instanceRoot);

} // namespace sage::ui

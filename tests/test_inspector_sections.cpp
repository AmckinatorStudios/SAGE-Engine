// ===========================================================================
//  РАЗДЕЛЫ ИНСПЕКТОРА ПО ТИПУ (часть 3): поля частей объявляют свой раздел
//  (PartField::Section), тип — свой главный раздел и порядок (Preset).
// ===========================================================================
#include "TestFramework.h"

#include <cstring>
#include <string>

#include "sage/ui/UI.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UIPresets.h"

namespace ui = sage::ui;

namespace {
const ui::PartField* Field(const char* part, const char* key) {
    const ui::PartType* p = ui::FindPart(part);
    if (!p || !p->Fields) return nullptr;
    for (const ui::PartField& f : *p->Fields)
        if (f.Key && std::strcmp(f.Key, key) == 0) return &f;
    return nullptr;
}
bool In(const char* part, const char* key, const char* section) {
    const ui::PartField* f = Field(part, key);
    if (!f) return false;
    if (!section) return f->Section == nullptr;
    return f->Section && std::strcmp(f->Section, section) == 0;
}
} // namespace

TEST(InspectorSections_progress_splits_value_look_and_transition) {
    // Progress: значение и режим — главный раздел; цвет заполнения —
    // оформление (рядом с дорожкой), скорость — переход.
    CHECK_TRUE(In("bar", "value", nullptr));
    CHECK_TRUE(In("bar", "kind", nullptr));
    CHECK_TRUE(In("bar", "indeterminate", nullptr));
    CHECK_TRUE(In("bar", "filled", ui::kSectionLook));
    CHECK_TRUE(In("bar", "source", ui::kSectionLook));
    CHECK_TRUE(In("bar", "smoothing", ui::kSectionTransition));
    CHECK_TRUE(In("bar", "easing", ui::kSectionTransition));
    // Переход — свой раздел, а не «Ещё настройки» внутри него.
    CHECK_TRUE(!Field("bar", "smoothing")->Advanced);
    const ui::Preset* p = ui::FindPreset("Progress");
    CHECK_TRUE(p && p->Section && std::string(p->Section) == "Progress");
    // Подложка шкалы — дорожка: два «Fill» в одном разделе больше нет.
    CHECK_TRUE(p->BackgroundTitle && std::string(p->BackgroundTitle) == "Track");
}

TEST(InspectorSections_text_has_content_typography_and_look) {
    CHECK_TRUE(In("label", "text", nullptr));
    CHECK_TRUE(In("label", "wrap", nullptr));
    CHECK_TRUE(In("label", "scale", ui::kSectionTypography));
    CHECK_TRUE(In("label", "horizontal", ui::kSectionTypography));
    CHECK_TRUE(In("label", "lineSpacing", ui::kSectionTypography));
    CHECK_TRUE(In("label", "color", ui::kSectionLook));
    CHECK_TRUE(In("label", "outline", ui::kSectionLook));
    const ui::Preset* t = ui::FindPreset("Text");
    CHECK_TRUE(t && t->ContentFirst && std::string(t->Section) == "Content");
}

TEST(InspectorSections_controls_keep_value_apart_from_look) {
    // Ползунок и переключатель: значение — Range / Value, дорожка, ручка,
    // квадратик и отметка — оформление.
    CHECK_TRUE(In("range", "value", nullptr));
    CHECK_TRUE(In("range", "step", nullptr));
    CHECK_TRUE(In("range", "variant", nullptr));
    CHECK_TRUE(In("range", "track", ui::kSectionLook));
    CHECK_TRUE(In("range", "knob", ui::kSectionLook));
    CHECK_TRUE(In("range", "box", ui::kSectionLook));
    CHECK_TRUE(In("range", "check", ui::kSectionLook));
    CHECK_TRUE(In("range", "animation", ui::kSectionTransition));
    CHECK_TRUE(std::string(ui::FindPreset("Slider")->Section) == "Range");
    CHECK_TRUE(std::string(ui::FindPreset("Toggle")->Section) == "Value");
    CHECK_TRUE(std::string(ui::FindPreset("Text Input")->Section) == "Input");
    CHECK_TRUE(std::string(ui::FindPreset("Dropdown")->Section) == "Options");
    CHECK_TRUE(In("textInput", "caretColor", ui::kSectionLook));
    CHECK_TRUE(In("dropdown", "listColor", ui::kSectionLook));
}

TEST(InspectorSections_button_caption_is_a_text_element) {
    // Надпись кнопки — объект «Текст» ТИПА Text: инспектор показывает ей
    // разделы текста (Content, Typography), а не безымянного «Текст».
    const ui::Preset* b = ui::FindPreset("Button");
    CHECK_TRUE(b && !b->Children.empty());
    CHECK_TRUE(b->Children[0].Type == "Text");
    // Сцены, где тип записан именем ребёнка, открываются с верным типом.
    CHECK_EQ(ui::CanonicalType("Текст"), std::string("Text"));
    CHECK_EQ(ui::CanonicalType("Button"), std::string("Button"));
    CHECK_EQ(ui::CanonicalType("MyGameWidget"), std::string("MyGameWidget"));   // свой тип игры не трогается
}

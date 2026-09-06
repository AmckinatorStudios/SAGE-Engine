#pragma once
#include <vector>

#include "ui/EditorPanel.h"

// ---------------------------------------------------------------------------
// Панель «Профилировщик» — то, чего у профилировщика не было: читателя.
//
// Механизм замеров (sage/core/Profiler.h) существовал целиком: метки времени
// GPU, отложенное чтение через три кадра, усреднение, честный отказ на
// драйвере без таймеров. Не существовало двух вещей — участков в кадре и
// того, кто показал бы результат. Поэтому SetEnabled(true) не вызывался НИ
// РАЗУ, и на вопрос «почему просело» ответить было нечем при полностью
// готовом для этого коде.
//
// Панель включает профилирование, ПОКА ОНА ОТКРЫТА, и выключает при закрытии:
// метки времени не бесплатны, и платить за них, когда на них никто не
// смотрит, незачем.
//
// ВТОРАЯ ПАНЕЛЬ НА SAGE UI. Взята второй намеренно: у неё таблица переменной
// длины со вложенностью и полосами — то, что в immediate-mode пишется
// табличным API, а в retained обязано собираться из обычных элементов. Если
// такое собирается без специальных средств, значит объектный слой годится не
// только для кнопок.
// ---------------------------------------------------------------------------
class ProfilerPanel : public EditorPanel {
public:
    const char* PanelId() const override { return "profiler"; }
    const char* Title() const override { return "Profiler"; }
    void Build(sage::ui::sui::UIContext& ui, sage::ui::sui::UIElement* root) override;
    void Sync(float dt) override;

    // Профилирование включается, пока панель показана. Зовёт оболочка: сама
    // панель не знает, видно её или нет.
    void SetShown(bool shown);

private:
    // Одна строка таблицы. Держим элементы, а не пересоздаём: список участков
    // почти всегда один и тот же, а числа в нём меняются каждый кадр.
    struct Row {
        sage::ui::sui::UIElement* Box = nullptr;
        sage::ui::sui::Label* Name = nullptr;
        sage::ui::sui::Label* Cpu = nullptr;
        sage::ui::sui::Label* Gpu = nullptr;
        sage::ui::sui::ProgressBar* Bar = nullptr;
    };

    Row& EnsureRow(size_t index);

    // Показывать усреднённые значения, а не покадровые. По умолчанию да:
    // покадровые скачут на десятки процентов от планировщика ОС, и читать их
    // невозможно — цифра меняется быстрее, чем взгляд её схватывает.
    bool m_averaged = true;
    bool m_wasEnabled = false;
    bool m_shown = false;

    sage::ui::sui::UIContext* m_ui = nullptr;
    sage::ui::sui::Label* m_frame = nullptr;
    sage::ui::sui::Label* m_hint = nullptr;
    sage::ui::sui::ScrollView* m_table = nullptr;
    std::vector<Row> m_rows;
};

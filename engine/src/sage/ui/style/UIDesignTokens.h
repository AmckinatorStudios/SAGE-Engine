#pragma once
#include <string>
#include <unordered_map>

#include "sage/ui/core/UITypes.h"

// ---------------------------------------------------------------------------
// ТОКЕНЫ ОФОРМЛЕНИЯ (§104 ТЗ) — именованные значения вместо чисел в узлах.
//
// ЗАЧЕМ. Интерфейс, у которого отступы записаны числами (12, 16, 24 — по вкусу
// в каждом узле), перестроить нельзя: «сделать всё чуть плотнее» означает
// пройти по трёмстам узлам. Токен даёт имя («Spacing.Medium»), а число живёт в
// одном месте — в теме.
//
// Токен — это ЗНАЧЕНИЕ, а не стиль: цвет, отступ, радиус, кегль, длительность.
// Из токенов собираются стили, из стилей — тема.
// ---------------------------------------------------------------------------
namespace sage::ui {

class UIDesignTokens {
public:
    void SetColor(const std::string& name, const UIColor& c) { m_colors[name] = c; }
    void SetNumber(const std::string& name, float v) { m_numbers[name] = v; }
    void SetString(const std::string& name, std::string v) { m_strings[name] = std::move(v); }

    // Значение по имени. Отсутствующий токен — не молчаливый ноль, а явно
    // заданный запасной вариант плюс возможность спросить, был ли он (§134).
    UIColor Color(const std::string& name, const UIColor& fallback = UIColor(1.0f)) const;
    float Number(const std::string& name, float fallback = 0.0f) const;
    std::string String(const std::string& name, const std::string& fallback = {}) const;
    bool Has(const std::string& name) const;

    const std::unordered_map<std::string, UIColor>& Colors() const { return m_colors; }
    const std::unordered_map<std::string, float>& Numbers() const { return m_numbers; }
    const std::unordered_map<std::string, std::string>& Strings() const { return m_strings; }

    void Clear();
    // Набор по умолчанию: нейтральная тёмная палитра, шкала отступов, радиусов
    // и кеглей. Не «дизайн движка», а рабочее начало, которое игра заменяет
    // своей темой целиком.
    static UIDesignTokens Default();

private:
    std::unordered_map<std::string, UIColor> m_colors;
    std::unordered_map<std::string, float> m_numbers;
    std::unordered_map<std::string, std::string> m_strings;
};

} // namespace sage::ui

#pragma once
#include <string>
#include <vector>

#include "sage/lang/SageLexer.h"

// ---------------------------------------------------------------------------
// SAGE Lua: СООБЩЕНИЯ КОМПИЛЯТОРА.
//
// Сообщение — не одна строка «Type mismatch», а заголовок и подробности:
// файл, строка и столбец, что ожидалось, что получено, к какому полю или
// методу это относится. Консоль редактора открывает файл по File:Line, а
// человек по тексту понимает, что исправить, не гадая.
//
//     Scripts/Player.slua:24:18
//     Ошибка типа:
//     Ожидалось значение number.
//     Получено значение string.
//     Поле: PlayerController.speed
// ---------------------------------------------------------------------------
namespace sage::lang {

struct Diagnostic {
    enum class Level { Error, Warning };
    Level Severity = Level::Error;
    std::string File;
    Pos P;
    std::string Title;               // «Ошибка типа», «Синтаксическая ошибка»…
    std::vector<std::string> Lines;  // подробности, по одной мысли на строку

    bool IsError() const { return Severity == Level::Error; }

    // Полный многострочный текст (как в примере выше).
    std::string Format() const {
        std::string out = File + ":" + std::to_string(P.Line) + ":" + std::to_string(P.Col) + "\n";
        out += (Severity == Level::Warning ? "Предупреждение: " : "") + Title + ":";
        for (const std::string& l : Lines) out += "\n" + l;
        return out;
    }
    // Одной строкой — для заголовка ошибки в консоли.
    std::string Short() const {
        std::string out = Title;
        for (const std::string& l : Lines) out += " " + l;
        return out;
    }
};

class DiagnosticSink {
public:
    explicit DiagnosticSink(std::string file) : m_file(std::move(file)) {}

    void Error(Pos p, std::string title, std::vector<std::string> lines = {}) {
        m_items.push_back({Diagnostic::Level::Error, m_file, p, std::move(title), std::move(lines)});
    }
    void Warning(Pos p, std::string title, std::vector<std::string> lines = {}) {
        m_items.push_back({Diagnostic::Level::Warning, m_file, p, std::move(title), std::move(lines)});
    }
    bool HasErrors() const {
        for (const Diagnostic& d : m_items)
            if (d.IsError()) return true;
        return false;
    }
    const std::string& File() const { return m_file; }
    std::vector<Diagnostic>& Items() { return m_items; }

private:
    std::string m_file;
    std::vector<Diagnostic> m_items;
};

} // namespace sage::lang

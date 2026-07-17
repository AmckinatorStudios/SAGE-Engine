#pragma once

// Визуальный стиль редактора SAGE: единая палитра/скругления/отступы + шрифт
// с кириллицей. Вынесено из EditorLayer, чтобы стиль правился в одном месте.
namespace EditorTheme {

// Полная настройка ImGuiStyle (цвета + метрики). Вызывать после CreateContext.
void Apply();

// Шрифт с кириллицей (assets/fonts/editor.ttf либо системный DejaVu/SegoeUI);
// без него русский текст рисуется как '???'. Вызывать до создания атласа.
void LoadFont();

} // namespace EditorTheme

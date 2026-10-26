#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""В меню и палитре команд редактора нет пунктов для разработчиков движка.

ПОЧЕМУ ЭТО ПРОВЕРКА.

Релиз собирается с main и уходит людям, которые делают игры. Пункт «Запустить
Lua-тесты» или «Лист иконок» нужен тому, кто проверяет сам редактор, а в меню
человека он только мешает: непонятно, что он делает, и страшно его нажать.
Такие пункты появляются незаметно — их добавляют «на время», для проверки, —
и потом едут в каждый релиз.

«Проверить скрипты» убрана тем же решением: ошибки скриптов и так видны в
«Проблемах» при запуске, а отдельная кнопка строгой проверки — инструмент
разработчика, а не человека, делающего игру.

Правило: подпись пункта меню (EditorIcons::MenuItem*, ImGui::MenuItem) и
команды палитры (m_commands.Add) не называет тесты, самопроверку, отладочный
лист или демо. Проверочные возможности остаются — их включают переменными
окружения (SAGE_EDITOR_SELFTEST, SAGE_EDITOR_ICON_SHEET), а не мышью.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
EDITOR = ROOT / "editor" / "src"

# Вызовы, у которых есть подпись для человека. Подпись — первая T("...") в
# пределах вызова: у команды палитры в аргументах лямбды со своими «;», так что
# конец вызова скобками не найти, а подпись всегда стоит в начале.
CALLS = re.compile(r'(EditorIcons::MenuItem\w*|ImGui::MenuItem|m_commands\.Add)\s*\(')
LABEL = re.compile(r'T\("([^"]*)"\)')
WINDOW = 240  # символов после открывающей скобки
# Слова, которые выдают пункт для разработчика движка.
FORBIDDEN = re.compile(r'\b(tests?|self-?test|icon sheet|demo|debug sheet|check scripts)\b', re.I)


def main() -> int:
    bad = []
    checked = 0
    for path in sorted(EDITOR.rglob("*.cpp")):
        if "selftest" in path.parts:
            continue  # сама самопроверка в меню не попадает
        text = path.read_text(encoding="utf-8")
        for m in CALLS.finditer(text):
            label_m = LABEL.search(text, m.end(), m.end() + WINDOW)
            if not label_m:
                continue
            checked += 1
            label = label_m.group(1)
            if FORBIDDEN.search(label):
                line = text.count("\n", 0, m.start()) + 1
                bad.append(f"{path.relative_to(ROOT)}:{line}: «{label}»")
    if bad:
        print("В меню или палитре команд — пункты для разработчика движка:")
        for b in bad:
            print("   ", b)
        print("Уберите пункт из меню; проверку включайте переменной окружения.")
        return 1
    print(f"Меню и палитра команд без проверочных пунктов: просмотрено подписей {checked}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

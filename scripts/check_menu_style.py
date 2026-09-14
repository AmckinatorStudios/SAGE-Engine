#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Всплывающее меню обязано объявить свои отступы (Sage::UI::MenuScope).

ПОЧЕМУ ЭТО ПРОВЕРКА, А НЕ ВНИМАТЕЛЬНОСТЬ.

ImGui берёт отступы всплывающего окна В МОМЕНТ BeginPopup — то есть те, что
протолкнуты вокруг вызова. Панель, которая перед этим сделала
PushStyleVar(WindowPadding, 0), отдаёт свой ноль ЛЮБОМУ меню, открытому изнутри
неё. Выглядит это так: пункты прижаты к самой рамке окна, без отступа с боков и
сверху, строки слиплись. Снаружи читается как «списки в редакторе сломаны» — и
правильно читается.

Так и случилось: строка инструментов вьюпорта рисуется с обнулённым
WindowPadding (кнопки обязаны сложиться в плотный ряд поверх сцены), и все три
меню, которые она открывает, приезжали без единого отступа.

Разглядеть это в коде нельзя: BeginPopup стоит в одной функции, а PushStyleVar —
в другой, иногда в другом файле. Поэтому правило простое и проверяемое: меню
САМО отвечает за свой вид. Перед BeginPopup/BeginPopupContext* в той же функции
обязан стоять Sage::UI::MenuScope — он возвращает отступы ТЕМЫ, какой бы стиль
ни был протолкнут вокруг.

МОДАЛЬНЫЕ ОКНА (BeginPopupModal) правило не касается: у них свой размер и свои
поля по смыслу — полоса прогресса, диалог создания проекта, подтверждение. Они
задают padding сами и осознанно.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = sorted((ROOT / "editor/src").rglob("*.cpp"))

# Начало функции верхнего уровня: строка без отступа, заканчивающаяся на «{».
FUNC_START = re.compile(r'^[A-Za-z_].*\)\s*(?:const\s*)?\{\s*$')
POPUP = re.compile(r'ImGui::(BeginPopup|BeginPopupContextItem|BeginPopupContextWindow|'
                   r'BeginPopupContextVoid)\s*\(')
SCOPE = re.compile(r'Sage::UI::MenuScope\s+\w+\s*;')


def main():
    problems = []
    checked = 0
    for path in SOURCES:
        lines = path.read_text(encoding="utf-8").split("\n")
        rel = path.relative_to(ROOT)
        # Разбор по функциям: у каждой смотрим, есть ли в ней MenuScope.
        start = None
        for i, line in enumerate(lines):
            if FUNC_START.match(line):
                start = i
            if not POPUP.search(line):
                continue
            checked += 1
            body = lines[start:i] if start is not None else lines[max(0, i - 40):i]
            if any(SCOPE.search(b) for b in body):
                continue
            problems.append(f"{rel}:{i + 1}: {line.strip()}")

    if problems:
        print("Меню без объявленных отступов — их вид зависит от того, кто их открыл:\n")
        for p in problems:
            print("  " + p)
        print("\nДобавьте `Sage::UI::MenuScope menu;` перед BeginPopup в той же функции.")
        print("Подробности — в шапке scripts/check_menu_style.py.")
        return 1

    print(f"Меню объявляют свои отступы: проверено всплывающих окон {checked}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

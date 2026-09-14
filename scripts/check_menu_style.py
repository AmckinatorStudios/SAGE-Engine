#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Всплывающее меню обязано объявить свои отступы (Sage::UI::MenuScope) —
и снять их РОВНО там, где заканчивается его собственный BeginPopup/EndPopup,
а не когда-нибудь потом.

ПОЧЕМУ ЭТО ПРОВЕРКА, А НЕ ВНИМАТЕЛЬНОСТЬ (часть первая — отступы).

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

ПОЧЕМУ ЭТО ПРОВЕРКА (часть вторая — время жизни).

Sage::UI::MenuScope — это RAII: отступы снимаются в деструкторе, когда
переменная выходит из СВОЕЙ области видимости C++, а не когда закрывается
ЕГО popup. Объявить его «голой» строкой на верхнем уровне функции —
`Sage::UI::MenuScope menu; if (BeginPopup(...)) {...}` — значит привязать
деструктор к КОНЦУ ФУНКЦИИ: если после этого if в той же функции есть ещё
код (ImGui::SameLine(), EndChild() соседнего списка и так далее), отступы
остаются протолкнутыми на стеке ImGui весь это код. Ровно это и произошло в
HierarchyPanel::Draw / AssetsPanel::Draw / ProjectLauncher::Draw / FileBrowser
/ InputPanel::Draw: ImGui при End()/EndChild() того окна видит лишние
непопнутые PushStyleVar и решает, что кадр интерфейса собран неверно
(«Missing PopStyleVar()»), а когда деструктор всё же срабатывает — уже вне
всякого настоящего окна — сыплет обратной ошибкой «PopStyleVar() too many
times!» на первое попавшееся окно. Видно это не сразу: пункты меню на экране
выглядят нормально, а сам редактор в это время фонит ошибками кадра и может
терять ввод (см. VIEWPORT_INPUT в самопроверке).

Поэтому у голого объявления `Sage::UI::MenuScope x;` (без if-инициализатора)
есть ровно один безопасный вид: после соответствующего EndPopup() и до конца
ТОГО ЖЕ БЛОКА, в котором x объявлен, не должно быть ничего, кроме закрывающих
скобок, пустых строк и комментариев — то есть деструктор обязан сработать
сразу же, без стороннего кода между ним и EndPopup(). Два способа этого
добиться:
  1. `if (Sage::UI::MenuScope x; ImGui::BeginPopup(...)) { ... }` — время
     жизни x привязано к самому if языком, а не порядком кода после него;
  2. отдельный блок `{ Sage::UI::MenuScope x; if (BeginPopup(...)) {...} }`.
Способ (1) короче и есть смысл предпочитать его новому коду.

МОДАЛЬНЫЕ ОКНА (BeginPopupModal) первую часть правила не касается: у них свой
размер и свои поля по смыслу — полоса прогресса, диалог создания проекта,
подтверждение. Они задают padding сами и осознанно.
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
END_POPUP = re.compile(r'ImGui::EndPopup\s*\(')
SCOPE_DECL = re.compile(r'Sage::UI::MenuScope\s+(\w+)\s*;')
SCOPE_INIT = re.compile(r'if\s*\(\s*Sage::UI::MenuScope\s+\w+\s*;')
# Разрешённый «хвост» после EndPopup() до конца блока: ничего, кроме пустых
# строк, комментариев и закрывающих скобок вложенных if/for — то есть кода,
# который ничего не делает с ImGui-окнами.
TAIL_OK = re.compile(r'^\s*(//.*)?\}*\s*$')


def brace_delta(line):
    # Грубо, но в этом коде фигурные скобки рядом с BeginPopup не прячутся ни
    # в строковых литералах, ни в символьных константах.
    code = line.split("//")[0]
    return code.count("{") - code.count("}")


def find_enclosing_block_end(lines, decl_idx):
    """Индекс строки, где закрывается блок, содержащий объявление decl_idx."""
    depth = 0
    for i in range(decl_idx, len(lines)):
        depth += brace_delta(lines[i])
        if depth < 0:
            return i
    return len(lines) - 1


def main():
    problems = []
    checked = 0
    for path in SOURCES:
        lines = path.read_text(encoding="utf-8").split("\n")
        rel = path.relative_to(ROOT)
        start = None
        for i, line in enumerate(lines):
            if FUNC_START.match(line):
                start = i
            if not POPUP.search(line):
                continue
            checked += 1
            lo = start if start is not None else max(0, i - 40)
            body = lines[lo:i]

            if SCOPE_INIT.search(line):
                continue  # время жизни привязано к if — безопасно по правилам языка

            decl_idx = None
            for j in range(len(body) - 1, -1, -1):
                if SCOPE_DECL.search(body[j]) and not SCOPE_INIT.search(body[j]):
                    decl_idx = lo + j
                    break

            if decl_idx is None:
                problems.append((f"{rel}:{i + 1}: {line.strip()}",
                                 "нет Sage::UI::MenuScope перед BeginPopup"))
                continue

            block_end = find_enclosing_block_end(lines, decl_idx)
            end_popup_idx = None
            for k in range(i, block_end + 1):
                if END_POPUP.search(lines[k]):
                    end_popup_idx = k
                    break
            if end_popup_idx is None:
                continue  # не наш BeginPopup (например, EndPopup в другой ветке) — не мешаем

            tail = lines[end_popup_idx + 1:block_end]
            if any(not TAIL_OK.match(t) for t in tail):
                name = SCOPE_DECL.search(lines[decl_idx]).group(1)
                problems.append((
                    f"{rel}:{decl_idx + 1}: Sage::UI::MenuScope {name};",
                    "объявлен не через if(...; BeginPopup...), а после EndPopup в том же "
                    "блоке есть ещё код — деструктор снимет отступы позже, чем закроется "
                    "текущее окно (см. шапку check_menu_style.py)"
                ))

    if problems:
        print("Проблемы с Sage::UI::MenuScope:\n")
        for where, why in problems:
            print(f"  {where}\n      {why}")
        print("\nПодробности — в шапке scripts/check_menu_style.py.")
        return 1

    print(f"Меню объявляют свои отступы и снимают их вовремя: проверено всплывающих окон {checked}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

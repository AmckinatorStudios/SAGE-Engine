#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Списки ImGui закрываются EndCombo, а кнопка со значком не получает подпись
вида «##имя».

ПОЧЕМУ ЭТО ПРОВЕРКА, А НЕ ВНИМАТЕЛЬНОСТЬ.

1. EndPopup вместо EndCombo. На вид одно и то же: EndCombo внутри и зовёт
   EndPopup. Разница в счётчике g.BeginComboDepth — по нему ImGui
   ПЕРЕИСПОЛЬЗУЕТ окна списков («##Combo_00», «##Combo_01», …). BeginCombo его
   увеличивает, EndCombo уменьшает, EndPopup — нет. То есть пока список открыт,
   счётчик растёт каждый кадр, и СЛЕДУЮЩИЙ список редактора закрывается с
   проверкой «Calling EndCombo() in wrong window!»: ImGui обрывает отрисовку
   окна. Снаружи это «выбрал интерфейс в списке — развалился весь редактор»,
   причём ломается не тот список, в котором ошибка.

2. Подпись «##имя» у EditorIcons::Button. Эта кнопка рисует подпись САМА
   (ImDrawList::AddText), а не через ImGui::Button, и правила «## скрывает
   текст» там нет: «##new_interface» печаталось рядом со значком. Хуже того,
   по этой же подписи считается ШИРИНА кнопки — кнопка на полторы сотни
   пикселей уезжала за правый край панели, где ImGui её обрезал, и нажатие до
   неё не доходило («кнопка не работает»). Кнопке без подписи есть свой вызов:
   EditorIcons::IconOnlyButton.

3. ImGui::SmallButton в редакторе. У неё нет вертикальных полей: подпись
   прижата к краям, кнопка ниже соседних полей и выглядит «сплющенной»
   («Сохранить этот вид как стиль», «Править по картинке…»). В редакторе
   кнопки одной высоты с полями — ImGui::Button или EditorIcons::Button.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = sorted(p for d in ("editor/src", "engine/src") for p in (ROOT / d).rglob("*.cpp"))

BEGIN_COMBO = re.compile(r'ImGui::BeginCombo\s*\(')
END_COMBO = re.compile(r'ImGui::EndCombo\s*\(')
END_POPUP = re.compile(r'ImGui::EndPopup\s*\(')
# EditorIcons::Button("значок", "##подпись" — подпись скрывать нечем.
ICON_BUTTON_HIDDEN = re.compile(r'EditorIcons::Button\s*\(\s*"[^"]*"\s*,\s*"##')
SMALL_BUTTON = re.compile(r'ImGui::SmallButton\s*\(')


def brace_delta(line):
    code = line.split("//")[0]
    return code.count("{") - code.count("}")


def main():
    problems = []
    combos = 0
    for path in SOURCES:
        lines = path.read_text(encoding="utf-8").split("\n")
        rel = path.relative_to(ROOT)
        for i, line in enumerate(lines):
            if SMALL_BUTTON.search(line.split("//")[0]) and str(rel).startswith("editor"):
                problems.append(f"{rel}:{i+1}: ImGui::SmallButton — сплющенная кнопка без "
                                f"вертикальных полей; в редакторе — ImGui::Button")
            if ICON_BUTTON_HIDDEN.search(line):
                problems.append(f"{rel}:{i+1}: кнопка со значком получила подпись «##…» — "
                                f"она будет напечатана и растянет кнопку; "
                                f"нужен EditorIcons::IconOnlyButton")
            if not BEGIN_COMBO.search(line):
                continue
            combos += 1
            # Тело списка: от строки с BeginCombo до закрытия её блока.
            depth = brace_delta(line)
            closed = False
            ends_with_combo = False
            j = i
            while j + 1 < len(lines) and depth > 0:
                j += 1
                if END_COMBO.search(lines[j]):
                    ends_with_combo = True
                elif END_POPUP.search(lines[j]) and not ends_with_combo:
                    problems.append(
                        f"{rel}:{j+1}: список (BeginCombo со строки {i+1}) закрыт EndPopup — "
                        f"нужен EndCombo, иначе ImGui теряет счётчик окон списков")
                    closed = True
                    break
                depth += brace_delta(lines[j])
            if not closed and not ends_with_combo and depth <= 0:
                problems.append(f"{rel}:{i+1}: у BeginCombo не видно EndCombo")

    for p in problems:
        print(p)
    if problems:
        print(f"ошибок: {len(problems)}")
        return 1
    print(f"ImGui: проверено списков {combos}, все закрыты EndCombo; "
          f"подписей «##» у кнопок со значком нет")
    return 0


if __name__ == "__main__":
    sys.exit(main())

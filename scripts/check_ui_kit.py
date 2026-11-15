#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Значки редактора — только Tabler через EditorIcons; инспекторы и окна
свойств — только контролами Editor UI Kit (editor/src/InspectorControls.h).

ПОЧЕМУ ЭТО ПРОВЕРКА, А НЕ ВНИМАТЕЛЬНОСТЬ.

1. Значки. Символ Юникода («▶», «✓», «→» в начале подписи), ASCII-«значок»
   («+», «-», «...», «x» на кнопке) или стрелка ImGui (ArrowButton) выглядят
   по-разному в каждом шрифте и теме, не подстраиваются под масштаб и цвет
   состояния и рядом со значками Tabler читаются как чужие. Нужного значка нет
   — его добавляют в scripts/gen_icon_font.py (и kNames в EditorIcons.cpp),
   а шрифт перегенерируют штатно; EditorIconFont.inl руками не правят.

2. Kit. Инспектор, где половина полей — kit, а половина — сырые ImGui::Combo
   (с отдельной квадратной стрелкой), ImGui::Checkbox и DragFloat3 (три поля
   с подписью справа), выглядит как два разных редактора на одной панели. В
   файлах инспекторов и окон свойств сырые виджеты, у которых в kit есть
   замена, запрещены:
     ImGui::Combo / BeginCombo     → ictl::Combo / BeginDropdown + EndDropdown
     ImGui::Checkbox               → ictl::Switch (с историей) или ictl::Check
     ImGui::DragFloat2/3/4         → ictl::DragVec / VectorN / Vector3
     ImGui::SeparatorText          → ictl::SeparatorText
     ImGui::TreeNode*              → ictl::SubSection / EndSubSection

3. Список имён значков в генераторе и в EditorIcons.cpp — один и тот же:
   имя, которого нет в шрифте, рисуется пустым квадратом.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EDITOR = ROOT / "editor" / "src"

# Файлы инспекторов и окон свойств, переведённые на kit.
KIT_FILES = sorted(
    set(EDITOR.glob("panels/InspectorPanel*.cpp"))
    | set(EDITOR.glob("UIElementProperties*.cpp"))
    | set(EDITOR.glob("sprite/*.cpp"))
    | {EDITOR / n for n in ("InterfaceProperties.cpp", "UIImageSource.cpp", "UIStyleOverride.cpp",
                            "PostChainUi.cpp", "ParticleUi.cpp", "VarsEditor.cpp", "ShaderParamsUi.cpp",
                            "SignalLinksEditor.cpp", "InspectorFields.cpp", "interface/InterfaceInspectorPanel.cpp")}
)
# Список категорий в окне «Добавить компонент» — дерево в меню, а не поле.
KIT_EXEMPT = {EDITOR / "panels" / "InspectorPanel_AddComponent.cpp"}

RAW = re.compile(r"ImGui::(Combo|BeginCombo|Checkbox|DragFloat[234]|DragInt[234]|SeparatorText|TreeNode|TreeNodeEx)\s*\(")
ARROW = re.compile(r"ImGui::ArrowButton\s*\(")
# Кнопка/пункт с подписью из одних знаков: «+», «-», «...», «x», «<», «>».
ASCII_ICON = re.compile(r'ImGui::(Button|SmallButton|Selectable|MenuItem|RadioButton)\s*\(\s*"'
                        r'(\+|-|x|X|\.\.\.|<|>|\^|v|\?|\*|=|o|O|<<|>>)(##[^"]*)?"')
# Символ-«значок» в начале строки или строкой целиком.
UNICODE_ICON = re.compile("[←-⇿⌀-⏿■-◿☀-➿⬀-⯿"
                          "\U0001F300-\U0001FAFF]")
STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')


def code_part(line):
    return line.split("//")[0]


def unicode_icon_in(literal):
    text = literal.strip()
    if not text:
        return False
    # Строка целиком из значка, или значок первым и за ним пробел: «▶ Play».
    if UNICODE_ICON.fullmatch(text[:1]) and (len(text) == 1 or text[1] == " "):
        return True
    return False


def icon_names_match(problems):
    gen = (ROOT / "scripts" / "gen_icon_font.py").read_text(encoding="utf-8")
    src = (EDITOR / "EditorIcons.cpp").read_text(encoding="utf-8")
    block = gen[gen.index("ICONS = {"):]
    block = block[:block.index("\n}\n")]
    gen_names = re.findall(r'^\s*"([a-z0-9-]+)"\s*:|,\s*"([a-z0-9-]+)"\s*:', block, re.M)
    gen_set = {a or b for a, b in gen_names}
    m = re.search(r"kNames\s*\[\s*\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        return
    src_set = set(re.findall(r'"([a-z0-9-]+)"', m.group(1)))
    for name in sorted(src_set - gen_set):
        problems.append(f"editor/src/EditorIcons.cpp: значок «{name}» есть в kNames, но не в "
                        f"scripts/gen_icon_font.py — в шрифте его нет")


def main():
    problems = []
    for path in sorted(EDITOR.rglob("*.cpp")) + sorted(EDITOR.rglob("*.h")):
        rel = path.relative_to(ROOT)
        lines = path.read_text(encoding="utf-8").split("\n")
        for i, line in enumerate(lines, 1):
            code = code_part(line)
            if ARROW.search(code):
                problems.append(f"{rel}:{i}: ImGui::ArrowButton — стрелка не из Tabler; "
                                f"EditorIcons::IconOnlyButton(\"chevron-down\", …) или список kit")
            if ASCII_ICON.search(code):
                problems.append(f"{rel}:{i}: подпись-«значок» из знаков; значок Tabler "
                                f"(EditorIcons / ictl::Button)")
            for m in STRING.finditer(code):
                if unicode_icon_in(m.group(1)):
                    problems.append(f"{rel}:{i}: символ Юникода вместо значка: \"{m.group(1)[:24]}\"")
            if path in KIT_FILES and path not in KIT_EXEMPT and RAW.search(code):
                problems.append(f"{rel}:{i}: сырой виджет ImGui в инспекторе — "
                                f"контрол Editor UI Kit (см. scripts/check_ui_kit.py)")
    icon_names_match(problems)
    if problems:
        print("\n".join(problems))
        print(f"\nзначки и kit: замечаний {len(problems)}")
        return 1
    print(f"значки и kit: файлов инспекторов {len(KIT_FILES)}, всё через Tabler и Editor UI Kit")
    return 0


if __name__ == "__main__":
    sys.exit(main())

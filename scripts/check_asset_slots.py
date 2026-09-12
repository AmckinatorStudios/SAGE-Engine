#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Ассет выбирается СЛОТОМ, а не набирается путём в поле ввода.

ЗАЧЕМ ЭТА ПРОВЕРКА СУЩЕСТВУЕТ. Поле ввода с путём — способ из тех времён,
когда редактор был надстройкой над текстовым файлом сцены. Оно требует ЗНАТЬ
путь наизусть и напечатать его без опечатки; не показывает, что именно
выбрано; не проверяет тип файла; молчит, когда файла нет; не принимает
перетаскивание. Каждое такое поле вдобавок живёт по своим правилам: одно ждёт
путь относительно проекта, другое абсолютный, третье применяется по Enter — и
набранное без Enter молча пропадает.

Так было в настройках неба: картинка неба, папка кубической карты и шесть
граней набирались руками, хотя ровно эти файлы лежат в панели ассетов рядом.

Правило: любая ссылка на ассет рисуется через assetslot::Draw (editor/src/
AssetSlot.h) — текстура, материал, модель, скрипт, шейдер, звук, клип, папка.
Слот показывает обложку, принимает бросок, отвечает цветом рамки ещё до
отпускания кнопки и сам говорит «файла нет».

ЧТО ИМЕННО ЛОВИТСЯ. ImGui::InputText рядом с именем, которое означает путь
(…Path, …Dir, …Folder, …File). Поля «имя», «поиск», «строка» проверка не
трогает: там нет пути.

ИСКЛЮЧЕНИЕ ОДНО ПО СМЫСЛУ — путь НАРУЖУ проекта. Папка проекта в лаунчере,
создание и открытие проекта, папка сборки: проекта ещё (или уже) нет, панели
ассетов нет, перетаскивать неоткуда. Там путь набирают или выбирают системным
диалогом, потому что другого источника не существует.

Запуск: python3 scripts/check_asset_slots.py
"""
import os
import re
import sys

ROOTS = ["editor/src"]

# Сам файловый диалог: он и есть то место, где путь набирают: строка «куда
# сохранить» и имя новой папки — его собственный инструмент, а не ссылка на
# ассет.
EXEMPT_FILES = {
    os.path.join("editor", "src", "FileBrowser.cpp"),
}

# Лаунчер целиком — названное исключение: он работает ДО того, как проект
# открыт, и ассетов ещё не существует.
EXEMPT_DIRS = (os.path.join("editor", "src", "ProjectLauncher"),)

# Пути НАРУЖУ проекта: их не из чего перетащить (см. заголовок).
EXEMPT_NAMES = {
    "m_projectDir",  # куда создать новый проект
    "m_openPath",    # какой проект открыть
    "m_buildDir",    # куда положить сборку игры
    "m_newFolder",   # имя создаваемой папки, а не ссылка на существующую
}

INPUT_TEXT = re.compile(r'\bImGui::InputText(?:Multiline|WithHint)?\s*\(')
PATHY = re.compile(r'\b([A-Za-z_][A-Za-z0-9_]*(?:Path|Paths|Dir|Dirs|Folder|File))\b')

# Сколько строк выше учитывать: буфер обычно заполняется за строку-две до
# самого вызова (snprintf(buf, ..., something.Path.c_str())).
LOOKBEHIND = 3


def exempt_file(full: str) -> bool:
    return full in EXEMPT_FILES or full.startswith(EXEMPT_DIRS)


def main() -> int:
    bad = []
    for root in ROOTS:
        for dirpath, _dirs, files in os.walk(root):
            for fn in sorted(files):
                if not fn.endswith((".cpp", ".h", ".hpp")):
                    continue
                # normpath обязателен: ROOTS написаны через «/», а исключения
                # собраны через os.path.join, и на Windows получалось
                # «editor/src\FileBrowser.cpp» против «editor\src\FileBrowser.cpp»
                # — то есть названное исключение там молча не срабатывало, и
                # проверка ругалась на файловый диалог, который сама же и
                # освобождает. На Linux разделитель один, поэтому CI этого не
                # видел.
                full = os.path.normpath(os.path.join(dirpath, fn))
                if exempt_file(full):
                    continue
                lines = open(full, encoding="utf-8", errors="replace").read().splitlines()
                for i, line in enumerate(lines):
                    if not INPUT_TEXT.search(line):
                        continue
                    if line.lstrip().startswith(("//", "*")):
                        continue  # про поля ввода как раз и пишут в комментариях
                    window = []
                    for l in lines[max(0, i - LOOKBEHIND):i + 1]:
                        if l.lstrip().startswith(("//", "*")):
                            continue
                        window.append(l)
                    for m in PATHY.finditer("\n".join(window)):
                        if m.group(1) in EXEMPT_NAMES:
                            continue
                        bad.append((full, i + 1, m.group(1), line.strip()))
                        break

    if not bad:
        print("Ассеты выбираются слотами: полей ввода с путём в редакторе нет.")
        return 0

    print("НАРУШЕНИЯ: путь к ассету набирается в поле ввода вместо слота.")
    print("Слот показывает обложку, принимает перетаскивание и проверяет тип;")
    print("поле ввода не делает ничего из этого (см. editor/src/AssetSlot.h).")
    print()
    for full, lineno, name, line in bad:
        print(f"  {full}:{lineno}: {name}")
        print(f"      {line}")
    print()
    print(f"Всего: {len(bad)}. Замените на assetslot::Draw(host, id, Kind::…, path, preview).")
    return 1


if __name__ == "__main__":
    sys.exit(main())

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

ВТОРОЕ ПРАВИЛО — ГРАНИЦА ДИАЛОГА «ОБЗОР…». Слот отвечает на вопрос «что
выбрано», а кнопка «Обзор…» рядом с ним открывает файловый диалог, и он
обязан быть заперт в проекте (FileBrowser::Config::Root). Иначе в слот
материала кладётся картинка из «Загрузок»: ссылка наружу работает ровно до
сборки игры и до переноса проекта на другую машину, а потом молча становится
пустым слотом — при том, что у человека файл на диске на месте.

Ловится так: диалог, который начинается в папке ассетов проекта или берёт
список расширений у слота (assetslot::Extensions), обязан в том же блоке
задать Root. Импорт, открытие проекта и папка сборки под это правило
не попадают: у них расширений ассета нет, они и смотрят НАРУЖУ.

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


# --- Второе правило: диалог выбора ассета заперт в проекте -------------------

# Блок настройки диалога: от «FileBrowser::Config» до его открытия.
CONFIG_BLOCK = re.compile(r'FileBrowser::Config\s+(\w+)\s*;(.*?)\.Open\(\s*\1\s*\)',
                          re.DOTALL)
# Признак «это выбор ассета проекта»: диалог начинается в папке ассетов, либо
# берёт список расширений у слота. Импорт, открытие проекта и папка сборки ни
# того, ни другого не делают — они и смотрят НАРУЖУ.
ASSET_MARKERS = (
    'assetslot::Extensions',
    'assetslot::ProjectRoot',
    'AssetsDir()',
)


def check_dialog_roots():
    """Диалоги выбора ассета, которым забыли поставить границу."""
    bad = []
    for root in ROOTS:
        for dirpath, _dirs, files in os.walk(root):
            for fn in sorted(files):
                if not fn.endswith((".cpp", ".h", ".hpp")):
                    continue
                full = os.path.normpath(os.path.join(dirpath, fn))
                if exempt_file(full):
                    continue
                src = open(full, encoding="utf-8", errors="replace").read()
                for m in CONFIG_BLOCK.finditer(src):
                    body = m.group(2)
                    if not any(mark in body for mark in ASSET_MARKERS):
                        continue   # это не выбор ассета: импорт, проект, сборка
                    if '.Root' in body:
                        continue
                    line = src.count('\n', 0, m.start()) + 1
                    bad.append((full, line, m.group(1)))
    return bad


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

    roots = check_dialog_roots()

    if not bad and not roots:
        print("Ассеты выбираются слотами: полей ввода с путём в редакторе нет,")
        print("диалоги выбора ассета заперты в проекте.")
        return 0

    if roots:
        print("НАРУШЕНИЯ: диалог выбора ассета не заперт в проекте.")
        print("Без FileBrowser::Config::Root в слот кладут файл из «Загрузок»,")
        print("и ссылка наружу ломается при сборке игры и переносе проекта.")
        print()
        for full, lineno, name in roots:
            print(f"  {full}:{lineno}: {name}.Root не задан")
        print()
        print("Добавьте: <cfg>.StartDir = <cfg>.Root = assetslot::ProjectRoot(host);")
        if not bad:
            return 1
        print()

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

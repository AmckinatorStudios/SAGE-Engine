#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Граница «окружение -> путь»: переменную окружения, в которой лежит ПУТЬ,
нельзя читать через std::getenv.

ЗАЧЕМ ЭТА ПРОВЕРКА СУЩЕСТВУЕТ. Редактор не запускался на русской Windows —
вообще, у всех, у кого имя пользователя написано кириллицей. Причина
умещается в одну строку:

    if (const char* appdata = std::getenv("APPDATA")) base = appdata;

Узкое окружение Windows отдаёт значения в ANSI-кодировке системы (CP1251 на
русской), а std::filesystem::path из узкой строки ждёт UTF-8. Увидев байты
CP1251, конструктор не портит имя и не возвращает пустой путь — он БРОСАЕТ
filesystem_error «Cannot convert character sequence: Illegal byte sequence».
Программа умирала до создания окна, а в логе оставались две строки.

Правило: такие переменные читаются через sage::EnvPath (даёт готовый path,
на Windows читая широкое окружение) или sage::EnvString (даёт UTF-8). Тогда
ANSI не попадает внутрь программы вовсе.

Проверка НЕ трогает переменные с числами и флагами (SAGE_MSAA, SAGE_VSYNC и
прочие): им кодировка безразлична, и заставлять их ходить через EnvString
значило бы менять полсотни строк ради ничего.

Запуск: python3 scripts/check_paths.py
"""
import os
import re
import sys

ROOTS = ["engine/src", "editor/src", "runtime/src", "games"]

# Где EnvPath и живёт: внутри него getenv — не нарушение, а реализация.
ALLOWED_FILES = {
    os.path.join("engine", "src", "sage", "core", "Paths.cpp"),
}

# Имя переменной означает ПУТЬ, если оканчивается так...
PATH_SUFFIXES = ("PATH", "DIR", "DIRS", "HOME", "PROFILE", "_TO", "FOLDER", "ROOT")
# ...или названо прямо здесь (имя о пути не говорит, а путь там лежит).
PATH_NAMES = {
    "APPDATA",
    "HOMEDRIVE",
    "SAGE_PROJECT",
    "SAGE_BLENDER",
    "SAGE_EDITOR_TEMPLATE_SHOTS",
    "SAGE_EDITOR_LOAD_MODELS",
    "SAGE_EDITOR_L10N_MISSING",
    "SAGE_EDITOR_OPEN_CODE",
    "SAGE_EDITOR_OPEN_PROJECT",
}

GETENV = re.compile(r'\bgetenv\s*\(\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*\)')

# ЗНАЧЕНИЕ берут — или только СПРАШИВАЮТ, задана ли переменная?
#
#     if (std::getenv("SAGE_PROFILE")) ...        <- спрашивают: перекодировать
#                                                    нечего, байты никуда не идут
#     const char* p = std::getenv("SAGE_...")     <- берут: вот это и опасно
#
# Проверка ловит второе. Первое запрещать бессмысленно: там нет строки, которая
# могла бы стать путём.
CAPTURED = re.compile(r'=\s*(?:std::)?getenv\s*\(')


def is_path_var(name: str) -> bool:
    return name in PATH_NAMES or name.endswith(PATH_SUFFIXES)


def main() -> int:
    bad = []
    for root in ROOTS:
        for dirpath, _dirs, files in os.walk(root):
            for fn in files:
                if not fn.endswith((".cpp", ".h", ".hpp", ".cc")):
                    continue
                full = os.path.join(dirpath, fn)
                if full in ALLOWED_FILES:
                    continue
                with open(full, encoding="utf-8", errors="replace") as f:
                    for lineno, line in enumerate(f, 1):
                        stripped = line.lstrip()
                        if stripped.startswith("//") or stripped.startswith("*"):
                            continue  # в комментариях про getenv как раз и пишут
                        if not CAPTURED.search(line):
                            continue
                        for m in GETENV.finditer(line):
                            if is_path_var(m.group(1)):
                                bad.append((full, lineno, m.group(1), line.rstrip()))

    if not bad:
        print("Граница «окружение -> путь» соблюдена: путей через getenv нет.")
        return 0

    print("НАРУШЕНИЯ: путь читается через getenv вместо sage::EnvPath/EnvString.")
    print("На Windows такое значение приходит в ANSI и валит fs::path исключением.")
    print()
    for full, lineno, name, line in bad:
        print(f"  {full}:{lineno}: {name}")
        print(f"      {line.strip()}")
    print()
    print(f"Всего: {len(bad)}. Замените на sage::EnvPath(\"ИМЯ\") (путь) либо")
    print("sage::EnvString(\"ИМЯ\") (строка UTF-8) — см. engine/src/sage/core/Paths.h.")
    return 1


if __name__ == "__main__":
    sys.exit(main())

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

ВТОРОЕ ПРАВИЛО — ОБРАТНЫЙ ХОД. Путь, собранный правильно, нельзя сплющивать
в узкую строку и строить из неё путь обратно:

    std::string StoragePath() { return sage::PathToUtf8(base / "projects.json"); }
    ...
    std::ifstream in(StoragePath());          <- вот здесь всё и терялось

UTF-8-строка, отданная узкому API Windows, читается как ANSI: у человека с
кириллицей в имени учётной записи получался ДРУГОЙ путь. Файл не читался и не
писался — молча, без единой ошибки. Так пропадали список проектов (стартовое
окно каждый раз показывало пустоту), выбранный язык и настройки редактора.

Правило: функция, отдающая путь, отдаёт std::filesystem::path. Строка — только
чтобы ПОКАЗАТЬ путь человеку, и открывать по ней файлы нельзя.

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


# --- Второе правило: строка-путь не открывается как файл ---------------------

# Объявление «строкой отдаём путь»: std::string ЧтоТоPath(...)
STRING_PATH_DECL = re.compile(r'\bstd::string\s+(\w*(?:Path|PathString))\s*\(')

# Где такую строку использовать нельзя: всё, что ОТКРЫВАЕТ или ТРОГАЕТ файл.
def use_patterns(name: str):
    n = re.escape(name)
    return [
        re.compile(r'\b(?:std::)?(?:i|o)?fstream\s+\w+\s*\(\s*(?:\w+::)?' + n + r'\s*\('),
        re.compile(r'\bfs::path\s+\w+\s*=\s*(?:\w+::)?' + n + r'\s*\('),
        re.compile(r'\b(?:std::filesystem|fs)::path\s*\(\s*(?:\w+::)?' + n + r'\s*\('),
        re.compile(r'\b(?:std::filesystem|fs)::(?:create_directories|exists|remove|remove_all|'
                   r'rename|copy_file|is_directory|file_size|last_write_time)\s*\(\s*'
                   r'(?:\w+::)?' + n + r'\s*\('),
    ]


def sources():
    for root in ROOTS:
        for dirpath, _dirs, files in os.walk(root):
            for fn in files:
                if fn.endswith((".cpp", ".h", ".hpp", ".cc")):
                    yield os.path.join(dirpath, fn)


def check_utf8_roundtrip():
    """Функции, отдающие путь строкой, и попытки открыть по ним файл."""
    names = set()
    texts = {}
    for full in sources():
        with open(full, encoding="utf-8", errors="replace") as f:
            texts[full] = f.read()
        names.update(STRING_PATH_DECL.findall(texts[full]))
    if not names:
        return []
    checks = [(n, pat) for n in names for pat in use_patterns(n)]
    bad = []
    for full, text in texts.items():
        for lineno, line in enumerate(text.splitlines(), 1):
            stripped = line.lstrip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            for name, pat in checks:
                if pat.search(line):
                    bad.append((full, lineno, name, line.rstrip()))
                    break
    return bad


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

    roundtrip = check_utf8_roundtrip()

    if not bad and not roundtrip:
        print("Граница «окружение -> путь» соблюдена: путей через getenv нет,")
        print("строковые пути файлами не открываются.")
        return 0

    if roundtrip:
        print("НАРУШЕНИЯ: файл открывается по ПУТИ-СТРОКЕ.")
        print("На Windows узкая строка читается как ANSI — путь с кириллицей")
        print("превращается в другой путь, и файл молча не читается и не пишется.")
        print()
        for full, lineno, name, line in roundtrip:
            print(f"  {full}:{lineno}: {name}() отдаёт std::string")
            print(f"      {line.strip()}")
        print()
        print("Отдавайте std::filesystem::path; строка — только чтобы ПОКАЗАТЬ путь.")
        if not bad:
            return 1
        print()

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

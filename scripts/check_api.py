#!/usr/bin/env python3
"""Сторож границы API: игра включает только публичное, редактор — не внутреннее.

ЗАЧЕМ. Игре был доступен любой заголовок движка, поэтому всё внутреннее
фактически считалось обещанием: переделать внутренний класс значило, возможно,
сломать чью-то игру. Обещание теперь записано явно в engine/api.txt (уровни
public / tools / внутреннее — там же объяснение), а этот скрипт держит его.

ЧТО ПРОВЕРЯЕТСЯ.
1. Манифест: каждая строка — существующий заголовок engine/src/sage, без
   повторов и без попадания в оба уровня.
2. Замкнутость: публичный заголовок включает только публичные, инструментальный
   — только публичные и инструментальные. Иначе внутреннее протекает через
   #include и становится обещанием без решения.
3. Потребители:
   - игры (tests/integration и каталоги из --game) — только public;
   - приложения движка (editor/src, runtime/src, tools/sage_api,
     tools/sage_build) — public и tools.
   Включение внутреннего заголовка sage/ или реализации вне sage/
   (engine/src/rhi, engine/src/physics: "rhi/...", "physics/...") — провал.
Модульные и кадровые тесты (tests/, кроме integration) — белый ящик, их
сторож не смотрит.

Игра в своём репозитории проверяется так же:
    python3 scripts/check_api.py --game path/to/MyGame/src
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "engine", "src")
SAGE = os.path.join(SRC, "sage")
MANIFEST = os.path.join(ROOT, "engine", "api.txt")

GAMES = ["tests/integration"]
TOOLS = ["editor/src", "runtime/src", "tools/sage_api", "tools/sage_build"]
# Реализации вне sage/: их заголовки — всегда внутреннее (их знает только app).
IMPL_DIRS = [d for d in os.listdir(SRC) if d != "sage" and os.path.isdir(os.path.join(SRC, d))]

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)
HEADER_EXT = (".h", ".hpp", ".inl")
SOURCE_EXT = HEADER_EXT + (".cpp", ".cc", ".mm")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def engine_headers():
    out = set()
    for d, _, files in os.walk(SAGE):
        for f in files:
            if f.endswith(HEADER_EXT):
                out.add(os.path.relpath(os.path.join(d, f), SRC).replace(os.sep, "/"))
    return out


def load_manifest(headers, errors):
    levels = {"public": set(), "tools": set()}
    level = None
    for n, raw in enumerate(read(MANIFEST).splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            level = line[1:-1]
            if level not in levels:
                errors.append(f"api.txt:{n}: неизвестный уровень [{level}]")
            continue
        if level not in levels:
            errors.append(f"api.txt:{n}: строка вне уровня [public]/[tools]")
            continue
        path = "sage/" + line
        if path not in headers:
            errors.append(f"api.txt:{n}: нет такого заголовка: {path}")
        elif path in levels["public"] | levels["tools"]:
            errors.append(f"api.txt:{n}: заголовок указан дважды: {path}")
        levels[level].add(path)
    return levels


def classify(inc, headers, levels):
    """Уровень включаемого файла движка; None — не файл движка (std, glm, свой)."""
    if inc in levels["public"]:
        return "public"
    if inc in levels["tools"]:
        return "tools"
    if inc in headers:
        return "internal"
    first = inc.split("/", 1)[0]
    if first in IMPL_DIRS and os.path.isfile(os.path.join(SRC, inc)):
        return "internal"
    return None


def resolve(inc, from_file):
    """Путь включения от engine/src. "Mesh.h" из sage/render/Model.h — это
    sage/render/Mesh.h: такие относительные включения в движке есть, и мимо них
    внутреннее протекло бы незамеченным."""
    near = os.path.normpath(os.path.join(os.path.dirname(from_file), inc))
    if os.path.isfile(near):
        rel = os.path.relpath(near, SRC).replace(os.sep, "/")
        if not rel.startswith(".."):
            return rel
    return inc


def scan(dirs):
    for rel in dirs:
        base = rel if os.path.isabs(rel) else os.path.join(ROOT, rel)
        if not os.path.isdir(base):
            continue
        for d, _, files in os.walk(base):
            for f in files:
                if f.endswith(SOURCE_EXT):
                    p = os.path.join(d, f)
                    yield p, read(p)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", action="append", default=[],
                    help="каталог кода игры (можно несколько): ей можно только public")
    args = ap.parse_args()

    headers = engine_headers()
    errors = []
    levels = load_manifest(headers, errors)
    allowed = {"public": {"public"}, "tools": {"public", "tools"}}

    # 2. Замкнутость уровней.
    for level in ("public", "tools"):
        for h in sorted(levels[level]):
            full = os.path.join(SRC, h)
            for inc in INCLUDE.findall(read(full)):
                inc = resolve(inc, full)
                got = classify(inc, headers, levels)
                if got and got not in allowed[level]:
                    errors.append(f"{h} ({level}) включает {inc} ({got}) — внутреннее "
                                  f"протекает наружу: поднимите {inc} в [{level}] "
                                  f"engine/api.txt или уберите включение")

    # 3. Потребители.
    for dirs, level, who in ((GAMES + args.game, "public", "игра"),
                             (TOOLS, "tools", "приложение движка")):
        for full, text in scan(dirs):
            path = os.path.relpath(full, ROOT).replace(os.sep, "/")
            for inc in INCLUDE.findall(text):
                inc = resolve(inc, full)
                got = classify(inc, headers, levels)
                if got and got not in allowed[level]:
                    errors.append(f"{path} ({who}) включает {inc} ({got}); "
                                  f"можно только {' и '.join(sorted(allowed[level]))}")

    pub, tools = len(levels["public"]), len(levels["tools"])
    print(f"API движка: публичных {pub}, для инструментов {tools}, "
          f"внутренних {len(headers) - pub - tools} (из {len(headers)})")
    if errors:
        for e in errors:
            print("ОШИБКА: " + e)
        print(f"check_api: нарушений {len(errors)} (правила — engine/api.txt)")
        return 1
    print("check_api: граница API соблюдена")
    return 0


if __name__ == "__main__":
    sys.exit(main())

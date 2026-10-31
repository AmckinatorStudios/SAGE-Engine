#!/usr/bin/env python3
"""Сторож слоёв движка: модуль включает только модули своего слоя и ниже.

ЗАЧЕМ. Модули движка (engine/src/sage/<модуль>) включали друг друга как
придётся, и это было видно по последствиям, а не по коду: базовый слой core
зависел от скриптов, физики и звука — то есть «поправить лог» значило
пересобрать и перепроверить весь движок, а игре, которой не нужна физика, её
было не отцепить. Граница RHI держится потому, что её сторожит скрипт
(check_rhi_boundary.py); здесь то же самое для всех модулей.

ПРАВИЛО. Слои снизу вверх (LAYERS). Включать можно свой слой и любой ниже.
Включение ВВЕРХ — нарушение. Внутри одного слоя модули видят друг друга
(слой «мир» пока связан плотно — это известный долг, см. docs/architecture.md).

УЖЕ СУЩЕСТВУЮЩИЕ нарушения записаны в scripts/layer_baseline.json — со счётом.
Новое нарушение (новая пара или больше включений, чем в базе) — провал.
Исправили — скрипт попросит уменьшить базу (--update перепишет её): база
обязана только уменьшаться.

Переходные заголовки-мосты (пометка SAGE_FORWARD_HEADER в файле) не
считаются: они указывают на НОВОЕ место переехавшего файла ради
совместимости игр и сами ничего не делают.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAGE = os.path.join(ROOT, "engine", "src", "sage")
BASELINE = os.path.join(ROOT, "scripts", "layer_baseline.json")

# Снизу вверх. Каждый модуль engine/src/sage/* обязан быть в одном из слоёв —
# новый модуль без слоя тоже провал: иначе он оказался бы вне правил.
LAYERS = [
    ("основа", ["rhi", "core"]),
    ("службы", ["events", "vars", "input", "physics", "ai"]),
    ("ресурсы", ["assets"]),
    ("мир", ["render", "ecs", "scene", "anim", "audio", "gi", "ui", "net"]),
    ("язык", ["lang"]),
    ("скрипты", ["scripting"]),
    ("программа", ["app"]),
]

INCLUDE = re.compile(r'^\s*#\s*include\s+"sage/([a-z_]+)/')


def layer_of():
    out = {}
    for i, (_, mods) in enumerate(LAYERS):
        for m in mods:
            out[m] = i
    return out


def scan():
    level = layer_of()
    mods = sorted(d for d in os.listdir(SAGE) if os.path.isdir(os.path.join(SAGE, d)))
    unknown = [m for m in mods if m not in level]
    edges = {}
    where = {}
    for m in mods:
        for dp, _, files in os.walk(os.path.join(SAGE, m)):
            for f in files:
                if not f.endswith((".h", ".hpp", ".cpp", ".inl")):
                    continue
                path = os.path.join(dp, f)
                with open(path, encoding="utf-8", errors="ignore") as fh:
                    text = fh.read()
                if "SAGE_FORWARD_HEADER" in text:
                    continue
                for line in text.splitlines():
                    mm = INCLUDE.match(line)
                    if not mm:
                        continue
                    dst = mm.group(1)
                    if dst == m or dst not in level or m not in level:
                        continue
                    if level[dst] > level[m]:
                        key = f"{m} -> {dst}"
                        edges[key] = edges.get(key, 0) + 1
                        where.setdefault(key, []).append(os.path.relpath(path, ROOT))
    return unknown, edges, where


def main():
    unknown, edges, where = scan()
    if "--update" in sys.argv:
        with open(BASELINE, "w", encoding="utf-8") as fh:
            json.dump(dict(sorted(edges.items())), fh, ensure_ascii=False, indent=2)
            fh.write("\n")
        print(f"база слоёв переписана: {len(edges)} пар, {sum(edges.values())} включений")
        return 0
    baseline = {}
    if os.path.exists(BASELINE):
        with open(BASELINE, encoding="utf-8") as fh:
            baseline = json.load(fh)
    failed = False
    if unknown:
        failed = True
        print("МОДУЛИ БЕЗ СЛОЯ (впишите их в LAYERS в scripts/check_layers.py):")
        for m in unknown:
            print(f"    {m}")
    grown = {k: v for k, v in edges.items() if v > baseline.get(k, 0)}
    if grown:
        failed = True
        print("ВКЛЮЧЕНИЕ ВВЕРХ ПО СЛОЯМ (нижний модуль не имеет права знать верхний):")
        for k, v in sorted(grown.items()):
            print(f"    {k}: {v} (в базе {baseline.get(k, 0)})")
            for p in where[k][:6]:
                print(f"        {p}")
        print("Нужное нижнему слою — объявить в нём (интерфейс, функция-крючок), а")
        print("реализацию держать наверху; см. docs/architecture.md, «Слои движка».")
    shrunk = {k: v for k, v in baseline.items() if edges.get(k, 0) < v}
    if shrunk and not failed:
        print("Нарушений стало меньше — уменьшите базу: python3 scripts/check_layers.py --update")
        for k, v in sorted(shrunk.items()):
            print(f"    {k}: было {v}, стало {edges.get(k, 0)}")
        failed = True
    if failed:
        return 1
    core = {k: v for k, v in edges.items() if k.split(" -> ")[0] in ("core", "rhi")}
    print(f"слои держатся: модулей {sum(len(m) for _, m in LAYERS)}, известных нарушений "
          f"{len(edges)} пар ({sum(edges.values())} включений), основа чиста: {'да' if not core else 'нет'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

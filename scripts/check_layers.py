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

ЦИКЛЫ ВНУТРИ СЛОЯ. Модули одного слоя видят друг друга, но взаимное
включение (A включает B, B включает A) — это уже не слой, а один модуль в двух
папках: ни один из них не вынести и не проверить отдельно. Цикл записывается
в базу как «A <-> B» со счётом включений в более редкую сторону (обратное
ребро). Новый цикл или рост обратного ребра — провал.

РЕАЛИЗАЦИИ ВНЕ sage/ (engine/src/physics, engine/src/rhi — бэкенды Jolt,
OpenGL и т. п.) тоже сканируются, у каждой свой слой (IMPLS). Их заголовки
включает только сборка программы (app), редактор и плеер: модуль движка,
который знает конкретный бэкенд, делает этот бэкенд незаменимым.

СБОРКА. Каждый слой — своя библиотека sage_<слой> (engine/CMakeLists.txt),
она линкует только слой под собой: вызов вверх без #include не соберётся.
Состав слоёв там (SAGE_LAYER_*, SAGE_IMPL_*) обязан совпадать с LAYERS и IMPLS
здесь — сторож сверяет.

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

# Реализации вне engine/src/sage: каталог -> слой. physics — мост физики со
# сценой (PhysicsScene читает компоненты сцены, меши и ассеты), поэтому «мир»,
# а не «службы»: в службах лежит только интерфейс sage/physics.
IMPLS = {"physics": "мир", "rhi": "основа"}
IMPL_USERS = {"app"}  # кому из модулей sage можно включать заголовки реализаций

INCLUDE = re.compile(r'^\s*#\s*include\s+"sage/([a-z_]+)/')
IMPL_INCLUDE = re.compile(r'^\s*#\s*include\s+"(' + "|".join(IMPLS) + r')/')


def layer_of():
    out = {}
    for i, (_, mods) in enumerate(LAYERS):
        for m in mods:
            out[m] = i
    return out


def impl_name(d):
    return f"{d}(реализация)"


def sources(top):
    for dp, _, files in os.walk(top):
        for f in files:
            if f.endswith((".h", ".hpp", ".cpp", ".inl")):
                path = os.path.join(dp, f)
                with open(path, encoding="utf-8", errors="ignore") as fh:
                    text = fh.read()
                if "SAGE_FORWARD_HEADER" not in text:
                    yield path, text


def scan():
    level = layer_of()
    names = [n for n, _ in LAYERS]
    mods = sorted(d for d in os.listdir(SAGE) if os.path.isdir(os.path.join(SAGE, d)))
    unknown = [m for m in mods if m not in level]
    tops = [(m, os.path.join(SAGE, m)) for m in mods]
    for d, layer in IMPLS.items():
        level[impl_name(d)] = names.index(layer)
        tops.append((impl_name(d), os.path.join(ROOT, "engine", "src", d)))
    edges = {}    # "a -> b": включений вверх по слоям
    same = {}     # (a, b): включений внутри слоя — для поиска циклов
    where = {}
    for m, top in tops:
        if m not in level:
            continue
        for path, text in sources(top):
            rel = os.path.relpath(path, ROOT)
            for line in text.splitlines():
                im = IMPL_INCLUDE.match(line)
                if im and not m.endswith("(реализация)") and m not in IMPL_USERS:
                    key = f"{m} -> {impl_name(im.group(1))}"
                    edges[key] = edges.get(key, 0) + 1
                    where.setdefault(key, []).append(rel)
                    continue
                mm = INCLUDE.match(line)
                if not mm:
                    continue
                dst = mm.group(1)
                if dst == m or dst not in level:
                    continue
                if level[dst] > level[m]:
                    key = f"{m} -> {dst}"
                    edges[key] = edges.get(key, 0) + 1
                    where.setdefault(key, []).append(rel)
                elif level[dst] == level[m]:
                    same[(m, dst)] = same.get((m, dst), 0) + 1
                    where.setdefault((m, dst), []).append(rel)
    # Цикл внутри слоя: счёт — включения в более редкую сторону (её и рвать).
    for (a, b), n in list(same.items()):
        back = same.get((b, a), 0)
        if a < b and back:
            key = f"{a} <-> {b}"
            edges[key] = min(n, back)
            rare = (a, b) if n <= back else (b, a)
            where[key] = [f"{rare[0]} -> {rare[1]}: {p}" for p in where[rare]]
    return unknown, edges, where


def cmake_layers():
    """Слои из engine/CMakeLists.txt: [(модули, реализации)] по порядку.

    Сборка держит слои библиотеками (sage_<слой>), сторож — включениями. Если
    списки разойдутся, один из них станет врать: модуль окажется в одном слое
    для компоновщика и в другом для сторожа. Поэтому сверяем.
    """
    path = os.path.join(ROOT, "engine", "CMakeLists.txt")
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    sets = dict(re.findall(r"^set\((SAGE_\w+)\s+([^)]*)\)", text, re.M))
    order = sets.get("SAGE_LAYERS", "").split()
    return [(sets.get(f"SAGE_LAYER_{n}", "").split(), sets.get(f"SAGE_IMPL_{n}", "").split()) for n in order]


def cmake_mismatch():
    want = [(mods, sorted(d for d, l in IMPLS.items() if l == name)) for name, mods in LAYERS]
    got = cmake_layers()
    if [(sorted(m), sorted(i)) for m, i in got] == [(sorted(m), i) for m, i in want]:
        return None
    return want, got


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
    mismatch = cmake_mismatch()
    if mismatch:
        failed = True
        print("СЛОИ В СБОРКЕ НЕ СОВПАДАЮТ С LAYERS (engine/CMakeLists.txt, SAGE_LAYER_*/SAGE_IMPL_*):")
        print(f"    сторож: {mismatch[0]}")
        print(f"    сборка: {mismatch[1]}")
    if unknown:
        failed = True
        print("МОДУЛИ БЕЗ СЛОЯ (впишите их в LAYERS в scripts/check_layers.py):")
        for m in unknown:
            print(f"    {m}")
    grown = {k: v for k, v in edges.items() if v > baseline.get(k, 0)}
    if grown:
        failed = True
        print("НАРУШЕНИЕ СЛОЁВ (включение вверх, заголовок реализации или цикл внутри слоя):")
        for k, v in sorted(grown.items()):
            print(f"    {k}: {v} (в базе {baseline.get(k, 0)})")
            for p in where[k][:6]:
                print(f"        {p}")
        print("Нужное нижнему слою — объявить в нём (интерфейс, функция-крючок), а")
        print("реализацию держать наверху. Цикл «A <-> B» рвут по более редкой")
        print("стороне тем же приёмом. См. docs/architecture.md, «Слои движка».")
    shrunk = {k: v for k, v in baseline.items() if edges.get(k, 0) < v}
    if shrunk and not failed:
        print("Нарушений стало меньше — уменьшите базу: python3 scripts/check_layers.py --update")
        for k, v in sorted(shrunk.items()):
            print(f"    {k}: было {v}, стало {edges.get(k, 0)}")
        failed = True
    if failed:
        return 1
    core = {k: v for k, v in edges.items() if k.split(" ")[0] in ("core", "rhi")}
    print(f"слои держатся: модулей {sum(len(m) for _, m in LAYERS)} (+{len(IMPLS)} реализации), известных нарушений "
          f"{len(edges)} пар ({sum(edges.values())} включений), основа чиста: {'да' if not core else 'нет'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

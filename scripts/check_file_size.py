#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Файл редактора — не длиннее LIMIT строк.

ЗАЧЕМ. Редактор рос файлами-«комбайнами»: самопроверка на восемь с половиной
тысяч строк, инспектор объекта, в котором одна функция рисовала двадцать
секций на полторы тысячи строк, запуск редактора, где обычный старт и
четыре сотни строк хуков для прогонов шли одной функцией. В таком файле правку
одной секции не отличить от правки соседней, каждое изменение пересобирает всё,
а «где это рисуется» ищут поиском, а не по имени файла.

Файл, выросший за предел, — сигнал разделить его ПО ОБЯЗАННОСТЯМ (секция,
этап, операция), а не по счёту строк: см. docs/architecture.md, «Модули
редактора».

УЖЕ СУЩЕСТВУЮЩИЕ длинные файлы записаны в scripts/file_size_baseline.json со
своей длиной. Им можно только уменьшаться: выросший — провал, ставший короче
предела — скрипт попросит убрать его из базы (--update перепишет её).

Сгенерированное (*.inl — переводы, шрифт значков) не считается.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "editor", "src")
BASELINE = os.path.join(ROOT, "scripts", "file_size_baseline.json")
LIMIT = 1000


def scan():
    sizes = {}
    for dp, _, files in os.walk(SRC):
        for f in files:
            if not f.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dp, f)
            with open(path, encoding="utf-8", errors="replace") as fh:
                n = sum(1 for _ in fh)
            sizes[os.path.relpath(path, ROOT).replace(os.sep, "/")] = n
    return sizes


def main():
    sizes = scan()
    over = {k: v for k, v in sizes.items() if v > LIMIT}
    if "--update" in sys.argv:
        with open(BASELINE, "w", encoding="utf-8") as fh:
            json.dump(dict(sorted(over.items())), fh, ensure_ascii=False, indent=2)
            fh.write("\n")
        print(f"база переписана: файлов длиннее {LIMIT} строк — {len(over)}")
        return 0
    baseline = {}
    if os.path.exists(BASELINE):
        with open(BASELINE, encoding="utf-8") as fh:
            baseline = json.load(fh)
    bad = {k: v for k, v in over.items() if v > baseline.get(k, LIMIT)}
    if bad:
        print(f"ФАЙЛ РЕДАКТОРА ДЛИННЕЕ {LIMIT} СТРОК — разделите его по обязанностям")
        print("(docs/architecture.md, «Модули редактора»):")
        for k, v in sorted(bad.items()):
            was = baseline.get(k)
            print(f"    {k}: {v}" + (f" (в базе {was})" if was else ""))
        return 1
    stale = {k: v for k, v in baseline.items() if sizes.get(k, 0) < v}
    if stale:
        print("файлы стали короче — обновите базу: python3 scripts/check_file_size.py --update")
        for k, v in sorted(stale.items()):
            print(f"    {k}: {sizes.get(k, 0)} (в базе {v})")
        return 1
    print(f"размер файлов редактора держится: проверено {len(sizes)}, длиннее {LIMIT} строк — "
          f"{len(over)} (все из базы, ни один не вырос)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

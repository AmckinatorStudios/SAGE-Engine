#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Сторож потоков: свои потоки заводит только система задач.

ЗАЧЕМ. До единой системы задач у движка было пять независимых пулов: пул
кадра, пул Jolt («ядра − 1»), поток декодирования текстур, пул задач скриптов
(«ядра − 2»), потоки упаковщика и запекания света — плюс по своему потоку у
миниатюр, превью, импорта, сборки игры и обложек проектов. На восьми ядрах
это три-четыре десятка потоков, которые ничего не знают ни о кадре, ни друг о
друге: планировщик ОС переключает их посреди кадра, а профилировщику их не
видно вовсе.

ПРАВИЛО. std::thread, std::jthread, std::async, pthread_create, CreateThread и
_beginthread вне engine/src/sage/jobs/ — провал. Фоновая работа — задача
(sage::jobs::Async / Run), с полосой (Worker, Background, IO, Main, Render),
подсистемой и, если нужно «не больше N разом», — sage::jobs::Throttle.

Тесты (tests/) сторож не смотрит: им нужны «чужие» потоки, чтобы проверить
систему задач снаружи.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ["engine/src", "editor/src", "runtime/src", "tools", "games"]
ALLOWED = [os.path.join("engine", "src", "sage", "jobs") + os.sep]
PATTERN = re.compile(
    r"std::thread\s*[({]|std::thread\s+\w+\s*[({]|std::vector<std::thread>|std::jthread|"
    r"std::async\s*\(|\bpthread_create\s*\(|\bCreateThread\s*\(|\b_beginthread(ex)?\s*\(")


def main():
    bad = []
    for d in DIRS:
        for dp, _, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                if not f.endswith((".h", ".hpp", ".cpp", ".inl", ".mm")):
                    continue
                path = os.path.join(dp, f)
                rel = os.path.relpath(path, ROOT)
                if any(rel.startswith(a) for a in ALLOWED):
                    continue
                with open(path, encoding="utf-8", errors="ignore") as fh:
                    for n, line in enumerate(fh, 1):
                        code = line.split("//", 1)[0]
                        if PATTERN.search(code):
                            bad.append(f"{rel}:{n}: {line.strip()}")
    if bad:
        print("Свои потоки вне системы задач (нужна задача sage::jobs, см. docs/multithreading.md):")
        for b in bad:
            print("  " + b)
        return 1
    print("потоки держатся: свои потоки заводит только система задач (engine/src/sage/jobs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Имена всплывающих окон ImGui: OpenPopup и BeginPopup* обязаны совпадать.

ПОЧЕМУ ЭТО ОТДЕЛЬНАЯ ПРОВЕРКА, А НЕ ВНИМАТЕЛЬНОСТЬ.

ImGui::OpenPopup только ПОМЕЧАЕТ окно открытым; показывает его тот, кто позовёт
BeginPopup/BeginPopupModal С ТЕМ ЖЕ ИМЕНЕМ. Разойдутся имена — окно остаётся в
стеке навсегда: рисовать его некому, а значит и закрыть некому.

И это не «диалог не показался». Тот, кто открыл, обычно просит КАЖДЫЙ КАДР —
флаг «надо спросить» сбрасывает сам диалог, а его нет. Каждый OpenPopup при
этом закрывает всё, что человек открыл выше по стеку (ImGui::OpenPopupEx ->
ClosePopupToLevel): меню «Файл» захлопывается в том же кадре, в котором
открылось, и ни одно меню и ни один диалог больше не открываются. Снаружи это
«редактор завис, кнопки не нажимаются» — при живой картинке и пустом логе.

Ровно так и было: у окна восстановления сцены стояло

    ImGui::OpenPopup(T("Restore scene?"));                       // имя ПЕРЕВЕДЁННОЕ
    ImGui::BeginPopupModal(T("Restore scene?" "###Restore scene?"), ...);

По-английски совпадало случайно (ImGui сбрасывает хэш на «###», и у строки без
«###» он совпадал с хвостом). По-русски OpenPopup получал «Восстановить
сцену?» — другой хэш, — и редактор после аварийного завершения переставал
слушаться мыши вовсе.

ЧТО ПРОВЕРЯЕТСЯ:

  1. Имя, отданное OpenPopup, должно встречаться у BeginPopup*. Сравниваются
     ИМЕННО ИДЕНТИФИКАТОРЫ: ImGui считает имя окна хэшем, сбрасывая его на
     «###», поэтому значение имеет хвост после последнего «###», а если его
     нет — вся строка.
  2. Имя, пропущенное через T(), ОБЯЗАНО содержать «###». Перевод меняет
     строку, а с ней и идентификатор окна: диалог, работавший по-английски,
     ломается ровно там, где интерфейс на другом языке.

Имена, собранные в рантайме (переменная, поле), проверить статически нечем — их
сторожит CloseGhostPopups в EditorLayer.cpp: окно, открытое без того, кто его
рисует, закрывается само, и в лог уходит строка с причиной.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = sorted(ROOT.glob("editor/src/**/*.cpp")) + sorted(ROOT.glob("editor/src/**/*.h"))

# "abc" "def" -> abcdef (соседние литералы склеивает компилятор, склеим и мы).
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
OPEN_CALL = re.compile(r'ImGui::OpenPopup\s*\(\s*([^;]*?)\s*\)\s*;')
BEGIN_CALL = re.compile(r'ImGui::(BeginPopupModal|BeginPopup|BeginPopupContextItem|'
                        r'BeginPopupContextWindow|BeginPopupContextVoid)\s*\(\s*([^;]*)')


def join_literals(text):
    """Склеенный C++-литерал из выражения. None — литерала в выражении нет."""
    parts = LITERAL.findall(text)
    if not parts:
        return None
    return "".join(parts)


def popup_id(name):
    """Идентификатор окна так, как его считает ImGui: хвост после «###»."""
    at = name.rfind("###")
    return name[at + 3:] if at >= 0 else name


def first_argument(text):
    """Первый аргумент вызова: до запятой на нулевом уровне вложенности."""
    depth = 0
    for i, ch in enumerate(text):
        if ch in "([":
            depth += 1
        elif ch in ")]":
            if depth == 0:
                return text[:i]
            depth -= 1
        elif ch == "," and depth == 0:
            return text[:i]
    return text


VAR_NAME = re.compile(r'([A-Za-z_]\w*)\s*(?:\.c_str\s*\(\s*\))?\s*$')


def variable_literal(text, arg):
    """Литерал из объявления переменной, переданной в BeginPopup*."""
    m = VAR_NAME.search(arg.strip())
    if not m:
        return None
    decl = re.search(r'\b' + re.escape(m.group(1)) + r'\s*=\s*([^;]*);', text)
    return join_literals(decl.group(1)) if decl else None


def main():
    opened = []      # (файл, строка, id, переведено ли, есть ли ###)
    begun = set()
    skipped = 0

    for path in SOURCES:
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(ROOT)
        for match in OPEN_CALL.finditer(text):
            expr = match.group(1)
            name = join_literals(expr)
            line = text.count("\n", 0, match.start()) + 1
            if name is None:
                skipped += 1
                continue
            opened.append((rel, line, popup_id(name), "T(" in expr, "###" in name))
        for match in BEGIN_CALL.finditer(text):
            arg = first_argument(match.group(2))
            name = join_literals(arg)
            if name is None:
                # Имя собрано в переменной («m_title + "###confirm"») — так
                # делают, когда подпись окна меняется, а идентификатор нет.
                # Берём литерал из объявления этой переменной: именно «###»-хвост
                # в нём и есть идентификатор окна.
                name = variable_literal(text, arg)
            if name is not None:
                begun.add(popup_id(name))

    problems = []
    for rel, line, ident, translated, has_hash in opened:
        if translated and not has_hash:
            problems.append(
                f"{rel}:{line}: ImGui::OpenPopup(T(\"{ident}\")) — имя переведено и без «###». "
                f"Идентификатор окна меняется вместе с языком: допишите «###{ident}»")
        elif ident not in begun:
            problems.append(
                f"{rel}:{line}: ImGui::OpenPopup с именем «{ident}» — окна с таким именем "
                f"никто не рисует (нет BeginPopup* с тем же «###»-хвостом)")

    if problems:
        print("Имена всплывающих окон разошлись — редактор от этого перестаёт "
              "слушаться мыши:\n")
        for p in problems:
            print("  " + p)
        print("\nПодробности — в шапке scripts/check_popup_ids.py.")
        return 1

    print(f"Всплывающие окна: проверено вызовов OpenPopup {len(opened)}, "
          f"имён у BeginPopup* {len(begun)}; "
          f"имена сходятся (в рантайме собрано {skipped} — их сторожит CloseGhostPopups).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

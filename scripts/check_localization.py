#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Проверка локализации редактора. Гоняется в CI вместе с остальными.

ЧТО ПРОВЕРЯЕТСЯ И ПОЧЕМУ ИМЕННО ЭТО.

1. НЕОБЁРНУТЫЕ СТРОКИ. Подпись, попавшая в ImGui мимо T(), выходит на экран
   по-английски при любом языке. Сборка на это не жалуется, тесты тоже: строка
   рабочая, просто не переведённая. Через десяток правок интерфейс снова
   становится наполовину английским — ровно то состояние, из которого его
   вытаскивали. Проверка ловит это в момент появления.

2. РАСХОЖДЕНИЕ СПЕЦИФИКАТОРОВ ФОРМАТА. Половина строк — форматы («Объектов:
   %zu»), и ImGui передаёт их в printf. Перевод, потерявший «%zu» или
   поменявший его на «%d», — это не опечатка, а чтение чужой памяти. Компилятор
   проверить это не может: он видит указатель, а не литерал. Здесь проверяется,
   что у ключа и перевода СОВПАДАЕТ последовательность спецификаторов.

3. МЁРТВЫЕ СТРОКИ КАТАЛОГА. Перевод, которому нет ключа в коде, — это работа
   впустую и подсказка, что строку в коде переименовали, а каталог забыли.
   Не ошибка, но сообщается.

4. УСТАРЕВШИЙ СГЕНЕРИРОВАННЫЙ КАТАЛОГ. Редактор читает переводы НЕ из JSON, а
   из lang_ru.inl, который делает scripts/gen_lang.py. Пока эти два файла
   расходятся, проверка выше отчитывается «все переведены», а на экране строки
   остаются английскими — и увидеть это можно только глазами, запустив
   редактор по-русски. Ровно так и случилось: в JSON накопилось 80 новых строк,
   которых в .inl не было, и переведённый заголовок «START FROM» выходил
   по-английски. Здесь сверяется, что .inl собран из текущего JSON.

    python3 scripts/check_localization.py
"""
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIRS = [os.path.join(REPO, 'editor', 'src'), os.path.join(REPO, 'editor', 'src', 'panels')]
CATALOG = os.path.join(REPO, 'editor', 'lang', 'ru.json')
# Тот самый файл, который редактор реально компилирует в себя (см. gen_lang.py).
GENERATED = os.path.join(REPO, 'editor', 'src', 'lang_ru.inl')

# Вызовы ImGui, у которых первый строковый аргумент виден на экране.
WIDGETS = """Text TextWrapped TextDisabled TextUnformatted Button SmallButton MenuItem BeginMenu
Checkbox Combo SliderFloat SliderInt SliderAngle DragFloat DragFloat2 DragFloat3 DragFloat4
DragInt InputText InputTextMultiline InputFloat InputInt InputDouble Selectable SetTooltip
SetItemTooltip ColorEdit3 ColorEdit4 RadioButton LabelText BulletText SeparatorText
InputTextWithHint Begin BeginChild BeginPopupModal BeginTabItem BeginTabBar CollapsingHeader
TreeNode TreeNodeEx""".split()

LITERAL = r'(?:\s*"(?:[^"\\]|\\.)*")+'
FMT = re.compile(r'%[-+ #0-9.*hlLqjzt]*[a-zA-Z%]')
TEXT_IN_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def visible_text(raw):
    """Видимая часть подписи: до «##» (дальше идёт скрытый идентификатор ImGui)."""
    return raw.split('##')[0]


ESCAPES = re.compile(r'\\[ntr\\"]')


def worth_translating(raw):
    text = visible_text(raw)
    stripped = FMT.sub('', text)
    # Экранированные последовательности — НЕ буквы. Без этого «%s\n\n%s» (чистый
    # формат, склеивающий три уже переведённых куска) считался подписью:
    # обратный слэш убирался, а «n» оставалась и проходила за букву.
    stripped = ESCAPES.sub('', stripped)
    if not re.search(r'[A-Za-zЀ-ӿ]', stripped):
        return False           # чистый идентификатор, формат или знаки
    return len(stripped.strip()) > 2   # «X», «Y», «Z» одинаковы во всех языках


def source_files():
    for d in SRC_DIRS:
        for name in sorted(os.listdir(d)):
            if name.endswith('.cpp') and not name.startswith('Localization'):
                yield os.path.join(d, name)


# Строки, которые ДВИЖОК отдаёт редактору на показ: названия частей элемента
# интерфейса, их поля и подсказки (см. SAGE_UI_TEXT в sage/ui/UIPart.h).
# Редактор рисует их через T(), но ключа-литерала у него нет — он приходит
# указателем. Без этого списка перевод новой части молча не появлялся бы, и
# заметить это можно было бы только глазами, открыв инспектор по-русски.
ENGINE_TEXT_FILES = [os.path.join(REPO, 'engine', 'src', 'sage', 'ui', 'UIParts.cpp')]
ENGINE_TEXT = re.compile(r'SAGE_UI_TEXT\(\s*"((?:[^"\\]|\\.)*)"\s*\)')


def engine_keys():
    keys = set()
    for path in ENGINE_TEXT_FILES:
        if not os.path.exists(path):
            continue
        with open(path, encoding='utf-8') as f:
            for m in ENGINE_TEXT.finditer(f.read()):
                keys.add(m.group(1))
    return keys


def collect():
    """(необёрнутые, ключи из T())"""
    unwrapped = []
    keys = set()
    call = re.compile(r'ImGui::(' + '|'.join(sorted(WIDGETS, key=len, reverse=True)) +
                      r')\s*\(\s*(' + LITERAL + r')')
    # У InputTextWithHint видима ПОДСКАЗКА (второй аргумент), а первый — всегда
    # скрытый идентификатор. Отдельным выражением, потому что общее правило
    # «первый строковый аргумент» здесь как раз не работает.
    hint = re.compile(r'ImGui::InputTextWithHint\s*\(\s*' + LITERAL + r'\s*,\s*(' + LITERAL + r')')
    twrap = re.compile(r'\bT\(\s*(' + LITERAL + r')\s*\)')
    for path in source_files():
        src = open(path, encoding='utf-8').read()
        rel = os.path.relpath(path, REPO)
        for m in call.finditer(src):
            raw = ''.join(TEXT_IN_LITERAL.findall(m.group(2)))
            if not worth_translating(raw):
                continue
            line = src.count('\n', 0, m.start()) + 1
            unwrapped.append('%s:%d: ImGui::%s("%s")' % (rel, line, m.group(1), raw[:60]))
        for m in hint.finditer(src):
            raw = ''.join(TEXT_IN_LITERAL.findall(m.group(1)))
            if not worth_translating(raw):
                continue
            line = src.count('\n', 0, m.start()) + 1
            unwrapped.append('%s:%d: подсказка InputTextWithHint("%s")' % (rel, line, raw[:60]))
        for m in twrap.finditer(src):
            raw = ''.join(TEXT_IN_LITERAL.findall(m.group(1)))
            if visible_text(raw):
                keys.add(visible_text(raw))
    return unwrapped, keys


def decode(text):
    """C-экранирование -> строка, как её увидит программа. Включая \\uXXXX:
    типографские кавычки в исходнике пишутся именно так."""
    out, i = [], 0
    while i < len(text):
        if text[i] == '\\' and i + 1 < len(text):
            if text[i+1] == 'u' and i + 5 < len(text):
                out.append(chr(int(text[i+2:i+6], 16)))
                i += 6
                continue
            out.append({'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\'}.get(text[i+1], text[i+1]))
            i += 2
        else:
            out.append(text[i]); i += 1
    return ''.join(out)


# Ключ так, как его записывает gen_lang.py в C-литерал. Сравнивать надо именно
# экранированный вид: в ключах есть переводы строк и кавычки.
def encode_c(text):
    out = text.replace('\\', '\\\\').replace('"', '\\"')
    out = out.replace('\n', '\\n').replace('\t', '\\t').replace('\r', '\\r')
    return '"' + out + '"'


def main():
    unwrapped, keys = collect()
    keys = {decode(k) for k in keys}
    # Плюс строки, которые движок отдаёт редактору на показ (SAGE_UI_TEXT).
    keys |= {decode(k) for k in engine_keys()}
    with open(CATALOG, encoding='utf-8') as f:
        strings = json.load(f).get('strings', {})

    failures = 0

    if unwrapped:
        print('НЕ ОБЁРНУТО в T() — эти подписи не переводятся:')
        for u in unwrapped:
            print('   ', u)
        failures += len(unwrapped)

    mismatched = []
    for key, value in strings.items():
        if FMT.findall(key) != FMT.findall(value):
            mismatched.append((key, value))
    if mismatched:
        print('СПЕЦИФИКАТОРЫ ФОРМАТА РАСХОДЯТСЯ (перевод уйдёт в printf как есть):')
        for key, value in mismatched:
            print('    %r -> %r' % (key, value))
        failures += len(mismatched)

    missing = sorted(k for k in keys if k not in strings)
    if missing:
        print('БЕЗ ПЕРЕВОДА (%d):' % len(missing))
        for k in missing:
            print('   ', repr(k))
        failures += len(missing)

    # Сгенерированный каталог обязан соответствовать JSON (см. пункт 4 в шапке).
    stale = []
    try:
        with open(GENERATED, encoding='utf-8') as f:
            generated = f.read()
    except OSError:
        generated = None
        print('НЕ НАЙДЕН %s — редактор соберётся без переводов' % GENERATED)
        failures += 1
    if generated is not None:
        for key in strings:
            if encode_c(key) not in generated:
                stale.append(key)
    if stale:
        print('СГЕНЕРИРОВАННЫЙ КАТАЛОГ УСТАРЕЛ (%d строк из JSON в нём нет).'
              % len(stale))
        print('    Редактор читает именно его — эти строки выйдут на экран по-английски.')
        print('    Лечится: python3 scripts/gen_lang.py')
        for k in stale[:10]:
            print('   ', repr(k))
        failures += len(stale)

    dead = sorted(k for k in strings if k not in keys)
    if dead:
        print('в каталоге есть строки, которых нет в коде (%d) — не ошибка, но повод убрать:'
              % len(dead))
        for k in dead[:20]:
            print('   ', repr(k))

    if failures:
        print('\nлокализация: замечаний %d' % failures)
        return 1
    print('локализация: строк в коде %d, все переведены, форматы совпадают' % len(keys))
    return 0


if __name__ == '__main__':
    sys.exit(main())

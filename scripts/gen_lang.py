#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Собирает встроенные каталоги перевода редактора из editor/lang/*.json.

ЗАЧЕМ ГЕНЕРАЦИЯ, А НЕ ЧТЕНИЕ ФАЙЛА НА СТАРТЕ. Редактор обязан запускаться из
любой папки и без ассетов рядом — иначе первый же запуск ярлыком показал бы
интерфейс без единого перевода, и выглядело бы это как поломка, а не как
отсутствие файла. Поэтому перевод вкомпилирован; внешний JSON (assets/lang/)
остаётся как способ ПОПРАВИТЬ строки без пересборки.

Источник правды — editor/lang/<код>.json, его правят переводом. Результат —
editor/src/lang_<код>.inl, его правят только этим скриптом.

    python3 scripts/gen_lang.py
"""
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(REPO, 'editor', 'lang')
OUT_DIR = os.path.join(REPO, 'editor', 'src')


def c_escape(text):
    """Строка -> тело литерала C++. Табуляция и перевод строки уезжают в \\n/\\t
    осознанно: настоящий перевод строки внутри литерала C++ недопустим."""
    out = []
    for ch in text:
        if ch == '\\': out.append('\\\\')
        elif ch == '"': out.append('\\"')
        elif ch == '\n': out.append('\\n')
        elif ch == '\t': out.append('\\t')
        elif ch == '\r': out.append('\\r')
        else: out.append(ch)
    return ''.join(out)


def generate(code):
    path = os.path.join(SRC_DIR, code + '.json')
    with open(path, encoding='utf-8') as f:
        root = json.load(f)
    strings = root.get('strings', {})

    lines = [
        '// СГЕНЕРИРОВАНО scripts/gen_lang.py из editor/lang/%s.json — не править руками.' % code,
        '// Правки вносятся в JSON, затем: python3 scripts/gen_lang.py',
        '//',
        '// Язык: %s. Строк: %d.' % (root.get('name', code), len(strings)),
        'struct TranslationPair {',
        '    const char* Key;',
        '    const char* Value;',
        '};',
        '',
        'constexpr TranslationPair kRussianStrings[] = {',
    ]
    for key in sorted(strings):
        lines.append('    {"%s",\n     "%s"},' % (c_escape(key), c_escape(strings[key])))
    lines.append('};')
    out = os.path.join(OUT_DIR, 'lang_%s.inl' % code)
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s -> %s (%d строк)' % (path, out, len(strings)))


if __name__ == '__main__':
    codes = sys.argv[1:] or ['ru']
    for code in codes:
        generate(code)

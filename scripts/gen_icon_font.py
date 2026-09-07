#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# Иконочный шрифт редактора: подмножество Tabler Icons -> C-массив.
#
# ЗАЧЕМ ГЕНЕРАТОР, А НЕ ПРОСТО ФАЙЛ В РЕПОЗИТОРИИ. Полный Tabler — это две с
# лишним тысячи килобайт и пять тысяч глифов, из которых редактору нужно
# шесть десятков. Скрипт вырезает ровно нужные и раскладывает их в заголовок,
# поэтому «добавить иконку» — это строка в таблице ниже и один прогон, а не
# ручная работа с бинарником, которую через полгода никто не повторит.
#
# ПОЧЕМУ МАССИВ, А НЕ ФАЙЛ РЯДОМ С EXE. Шрифт, лежащий файлом, — это ещё один
# путь, который может не найтись: ровно на таких путях редактор и спотыкался
# (модель, текстура, звук). Встроенный массив не теряется и не зависит от того,
# из какой папки запущен редактор.
#
# Запуск (нужны fonttools и распакованный @tabler/icons-webfont):
#   python3 scripts/gen_icon_font.py <путь к tabler-icons-outline.ttf> \
#                                    <путь к tabler-icons-outline.css>
# ---------------------------------------------------------------------------
import json, os, re, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, 'editor', 'src', 'EditorIconFont.inl')

# Имя иконки в редакторе -> имя иконки в Tabler. Порядок и состав должны
# совпадать с kNames в EditorIcons.cpp: имена редактора — про СМЫСЛ («материал»,
# «префаб»), имена Tabler — про рисунок, и связывать их надо явно.
ICONS = {
    "play": "player-play", "pause": "player-pause", "stop": "player-stop",
    "step": "player-track-next",
    "move": "arrows-move", "rotate": "rotate-3d", "scale": "resize",
    "universal": "vector", "rect": "crop", "align": "layout-align-left", "drop": "droplet",
    "grid": "grid-dots", "wire": "vector-triangle",
    "cube": "cube", "sphere": "sphere", "light": "bulb", "sun": "sun", "camera": "camera",
    "script": "file-code", "particles": "sparkles", "anim": "walk", "ik": "body-scan",
    "probe": "circle-dot", "network": "network", "physics": "atom",
    "folder": "folder", "file": "file", "scene": "stack-2", "material": "palette",
    "project": "briefcase", "prefab": "box-multiple", "texture": "photo", "shader": "brush",
    "audio": "volume", "model": "cube-3d-sphere",
    "up": "arrow-up", "refresh": "refresh", "folder-plus": "folder-plus", "search": "search",
    "clock": "clock", "list": "list", "import": "file-import", "pencil": "pencil",
    "code": "code", "question": "help", "layout": "layout", "gear": "settings",
    "magnet": "magnet",
    "warn": "alert-triangle", "error": "circle-x", "info": "info-circle", "debug": "bug",
    "trash": "trash", "copy": "copy", "save": "device-floppy", "open": "folder-open",
    "plus": "plus", "eye": "eye", "lock": "lock",
}


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    ttf, css_path = sys.argv[1], sys.argv[2]
    css = open(css_path, encoding='utf-8').read()
    table = dict(re.findall(r'\.ti-([a-z0-9-]+):before\s*\{\s*content:\s*"\\([0-9a-f]+)";', css))

    missing = sorted(n for n in ICONS.values() if n not in table)
    if missing:
        print('нет в шрифте:', ', '.join(missing))
        return 1

    points = {name: int(table[tabler], 16) for name, tabler in ICONS.items()}
    with tempfile.TemporaryDirectory() as tmp:
        uni = os.path.join(tmp, 'unicodes.txt')
        open(uni, 'w').write(','.join('U+%04X' % c for c in sorted(set(points.values()))))
        out = os.path.join(tmp, 'subset.ttf')
        subprocess.run(['pyftsubset', ttf, '--unicodes-file=' + uni, '--output-file=' + out,
                        '--no-hinting', '--desubroutinize', '--drop-tables+=DSIG'], check=True)
        data = open(out, 'rb').read()

    lines = []
    lines.append('// СГЕНЕРИРОВАНО scripts/gen_icon_font.py — РУКАМИ НЕ ПРАВИТЬ.')
    lines.append('//')
    lines.append('// Подмножество Tabler Icons (%d глифов, %d байт), MIT.' % (len(points), len(data)))
    lines.append('// https://tabler.io — лицензия в external/tabler-icons-LICENSE.')
    lines.append('#pragma once')
    lines.append('')
    lines.append('namespace EditorIconFont {')
    lines.append('')
    lines.append('inline const unsigned char kTablerSubset[] = {')
    for i in range(0, len(data), 16):
        lines.append('    ' + ' '.join('0x%02x,' % b for b in data[i:i + 16]))
    lines.append('};')
    lines.append('inline constexpr unsigned int kTablerSubsetSize = %d;' % len(data))
    lines.append('')
    lines.append('// Имя иконки редактора -> кодовая точка в шрифте.')
    lines.append('struct Glyph { const char* Name; unsigned int Code; };')
    lines.append('inline const Glyph kGlyphs[] = {')
    for name in sorted(points):
        lines.append('    {"%s", 0x%04x},' % (name, points[name]))
    lines.append('};')
    lines.append('inline constexpr int kGlyphCount = %d;' % len(points))
    lines.append('')
    lines.append('} // namespace EditorIconFont')
    lines.append('')
    open(OUT, 'w', encoding='utf-8').write('\n'.join(lines))
    print('%s: %d глифов, %d байт шрифта' % (OUT, len(points), len(data)))
    return 0


if __name__ == '__main__':
    sys.exit(main())

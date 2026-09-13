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
#   python3 scripts/gen_icon_font.py <распакованный .../dist/fonts> <.../dist>
#
# Берутся ОБА начертания: контурное (основное) и залитое — оно нужно там, где
# смысл передаётся парой «контур/заливка», например пустая папка против папки с
# содержимым.
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
    # Выбор — КУРСОР. Инструмент «просто выделять» ничего не делает с объектом,
    # и рисунок обязан говорить ровно это: стрелка мыши, а не ещё один
    # манипулятор.
    "select": "pointer",
    # Поворот — КРУГОВАЯ СТРЕЛКА, а не кубик с дугой: на шестнадцати пикселях
    # кубик превращается в пятно, и в ряду рядом с «перенести» и «масштаб» он
    # читается как ещё один объект, а не как действие над ним.
    "move": "arrows-move", "rotate": "rotate-clockwise", "scale": "resize",
    "universal": "vector", "rect": "crop", "align": "layout-align-left", "drop": "droplet",
    # Пространство осей: мир — глобус, объект — куб. Пара читается без подписи.
    "world": "world",
    # Многоточие — «здесь есть ещё»: общепринятый знак спрятанного меню.
    "dots": "dots",
    # «Взять управление» — джойстик. Ни камера (она уже стоит в заголовке
    # карточки), ни стрелки перемещения (это гизмо) здесь не годятся: кнопка
    # означает не «камера» и не «двигать», а «рулить этим отсюда».
    "pilot": "device-gamepad",
    "grid": "grid-dots", "wire": "vector-triangle",
    "cube": "cube", "sphere": "sphere", "light": "bulb", "sun": "sun", "camera": "camera",
    "script": "file-code", "particles": "sparkles", "anim": "walk", "ik": "body-scan",
    "probe": "circle-dot", "network": "network", "physics": "atom",
    "folder": "folder", "file": "file", "scene": "stack-2", "material": "palette",
    # Папка с содержимым — ЗАЛИТАЯ. Пара «контур/заливка» читается мгновенно и
    # не требует подписи, в отличие от «folder» против «folder-open»: открытая
    # папка означает «в неё вошли», а не «в ней что-то есть».
    "folder-full": ("filled", "folder"),
    "project": "briefcase", "prefab": "box-multiple", "texture": "photo", "shader": "brush",
    "audio": "volume", "model": "cube-3d-sphere",
    "up": "arrow-up", "refresh": "refresh",
    # Отмена и повтор — гнутые стрелки, как во всех редакторах. Прямые
    # («влево»/«вправо») читаются как переход по списку, а не как отмена.
    "undo": "arrow-back-up", "redo": "arrow-forward-up",
    # Конус — ПРОЖЕКТОР. Раньше его роль играла капля: она сужается книзу и
    # потому хоть как-то читалась, но конус — это буквально форма его светового
    # пучка, и объяснять её не нужно.
    "cone": "cone-2",
    # Капсула — рабочая форма персонажа (см. BuildCapsule): в списке форм у неё
    # обязан быть свой значок, а не общий кубик.
    "capsule": "capsule", "folder-plus": "folder-plus", "search": "search",
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
    # Аргументы — КАТАЛОГИ распакованного @tabler/icons-webfont: dist/fonts и dist.
    fonts_dir, css_dir = sys.argv[1], sys.argv[2]

    def table_of(style):
        name = 'tabler-icons-%s.css' % style
        css = open(os.path.join(css_dir, name), encoding='utf-8').read()
        return dict(re.findall(r'\.ti-([a-z0-9-]+):before\s*\{\s*content:\s*"\\([0-9a-f]+)";',
                               css))

    tables = {'outline': table_of('outline'), 'filled': table_of('filled')}

    # Имя иконки редактора -> (начертание, кодовая точка).
    points, missing = {}, []
    for name, want in ICONS.items():
        style, tabler = want if isinstance(want, tuple) else ('outline', want)
        code = tables[style].get(tabler)
        if code is None:
            missing.append('%s (%s/%s)' % (name, style, tabler))
            continue
        points[name] = (style, int(code, 16))
    if missing:
        print('нет в шрифте:', ', '.join(missing))
        return 1

    # ДВА НАЧЕРТАНИЯ — ДВА ШРИФТА, и подмножества из них сливаются в один.
    # Кодовые точки у контурного и залитого наборов разные, поэтому слияние
    # безопасно: глифы не накладываются друг на друга.
    with tempfile.TemporaryDirectory() as tmp:
        parts = []
        for style in ('outline', 'filled'):
            codes = sorted({c for st, c in points.values() if st == style})
            if not codes:
                continue
            uni = os.path.join(tmp, style + '.txt')
            open(uni, 'w').write(','.join('U+%04X' % c for c in codes))
            out = os.path.join(tmp, style + '.ttf')
            subprocess.run(['pyftsubset', os.path.join(fonts_dir, 'tabler-icons-%s.ttf' % style),
                            '--unicodes-file=' + uni, '--output-file=' + out,
                            '--no-hinting', '--desubroutinize', '--drop-tables+=DSIG'], check=True)
            parts.append(out)
        if len(parts) == 1:
            data = open(parts[0], 'rb').read()
        else:
            from fontTools.merge import Merger
            merged = os.path.join(tmp, 'merged.ttf')
            Merger().merge(parts).save(merged)
            data = open(merged, 'rb').read()

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
        lines.append('    {"%s", 0x%04x},' % (name, points[name][1]))
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

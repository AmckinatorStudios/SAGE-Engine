#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sage-icon-builder — сборщик значков SAGE (ТЗ «SAGE Icon System»).

    Tabler SVG  ->  разбор путей  ->  растеризация  ->  атлас  ->  enum + реестр

ЧТО ОН ДЕЛАЕТ И ПОЧЕМУ ИМЕННО ТАК.

Значки в интерфейсе — это сотни маленьких картинок, и почти всё, что с ними
делают неправильно, стоит дорого именно в мелочах:

  • держать папку с тысячами SVG и читать их при запуске — это тысячи обращений
    к диску и разбор XML ради пары штрихов;
  • заводить по текстуре на значок — это сотни привязок текстуры за кадр, то
    есть сотни вызовов рисования там, где хватило бы одного;
  • хранить по картинке на каждый цвет состояния — это шестикратный расход
    памяти ради того, что делается умножением в шейдере.

Поэтому: РОВНО нужные значки (список в engine/icons/icons.json) собираются
здесь, на этапе сборки, в ОДИН атлас покрытия (R8, как у шрифта), а рантайм
получает готовые пиксели и таблицу координат. Исходные SVG после сборки не
нужны — Tabler остаётся исходным ресурсом, а не зависимостью рантайма.

РАСТЕРИЗАЦИЯ БЕЗ ВНЕШНИХ БИБЛИОТЕК. Значки Tabler — это только обводка: замкнутых
заливок в наборе outline нет, есть ломаные и кривые с круглыми концами и
стыками. А обводка круглым пером — это в точности множество точек, удалённых от
линии не дальше половины толщины. Значит покрытие пикселя считается как
расстояние до ломаной, и сглаживание получается точным, а не «по четырём
подвыборкам»: круглые концы и стыки выходят сами собой, без единого особого
случая.

Запуск:
    python3 scripts/sage_icon_builder.py [--check]

--check ничего не пишет, а только сверяет, что сгенерированное соответствует
манифесту и исходникам. Для проверки в CI: сгенерированный файл, разошедшийся
с манифестом, — это значок, которого в коде ждут, а в атласе нет.
"""

import json
import math
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(REPO, 'engine', 'icons', 'icons.json')
OUT_DIR = os.path.join(REPO, 'engine', 'src', 'sage', 'ui', 'icons')
OUT_HEADER = os.path.join(OUT_DIR, 'SageIcons.generated.h')
OUT_SOURCE = os.path.join(OUT_DIR, 'SageIcons.generated.cpp')


# ---------------------------------------------------------------------------
#  Разбор SVG
# ---------------------------------------------------------------------------

NUM = re.compile(r'[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?')


def tokenize(d):
    """Команды и числа пути в порядке появления."""
    out = []
    i = 0
    while i < len(d):
        c = d[i]
        if c.isalpha():
            out.append(c)
            i += 1
        elif c in ' ,\t\r\n':
            i += 1
        else:
            m = NUM.match(d, i)
            if not m:
                raise ValueError('не разбирается путь у символа %d: %r' % (i, d[i:i + 12]))
            out.append(float(m.group()))
            i = m.end()
    return out


def flatten_cubic(p0, p1, p2, p3, out, steps):
    for k in range(1, steps + 1):
        t = k / steps
        u = 1.0 - t
        x = (u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0])
        y = (u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1])
        out.append((x, y))


def flatten_arc(p0, rx, ry, rot, large, sweep, p1, out, steps):
    """Дуга из конечных точек в центровую параметризацию (SVG, приложение F.6)."""
    if rx == 0 or ry == 0 or (abs(p0[0] - p1[0]) < 1e-9 and abs(p0[1] - p1[1]) < 1e-9):
        out.append(p1)
        return
    rx, ry = abs(rx), abs(ry)
    phi = math.radians(rot)
    cosp, sinp = math.cos(phi), math.sin(phi)
    dx2, dy2 = (p0[0] - p1[0]) / 2.0, (p0[1] - p1[1]) / 2.0
    x1 = cosp * dx2 + sinp * dy2
    y1 = -sinp * dx2 + cosp * dy2

    # Радиусы, которых не хватает, чтобы соединить концы, увеличиваются — так
    # требует сама спецификация; иначе корень ниже станет отрицательным.
    lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry)
    if lam > 1.0:
        s = math.sqrt(lam)
        rx *= s
        ry *= s

    num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1
    den = rx * rx * y1 * y1 + ry * ry * x1 * x1
    co = math.sqrt(max(0.0, num / den)) if den > 0 else 0.0
    if large == sweep:
        co = -co
    cx1 = co * rx * y1 / ry
    cy1 = -co * ry * x1 / rx
    cx = cosp * cx1 - sinp * cy1 + (p0[0] + p1[0]) / 2.0
    cy = sinp * cx1 + cosp * cy1 + (p0[1] + p1[1]) / 2.0

    def angle(ux, uy, vx, vy):
        dot = ux * vx + uy * vy
        n = math.hypot(ux, uy) * math.hypot(vx, vy)
        a = math.acos(max(-1.0, min(1.0, dot / n))) if n else 0.0
        return -a if ux * vy - uy * vx < 0 else a

    theta = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry)
    delta = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry)
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    elif sweep and delta < 0:
        delta += 2 * math.pi

    n = max(2, int(steps * abs(delta) / math.pi))
    for k in range(1, n + 1):
        a = theta + delta * k / n
        x = cosp * rx * math.cos(a) - sinp * ry * math.sin(a) + cx
        y = sinp * rx * math.cos(a) + cosp * ry * math.sin(a) + cy
        out.append((x, y))


def parse_path(d, steps=12):
    """Путь -> список ломаных (каждая — список точек)."""
    toks = tokenize(d)
    polys = []
    cur = []
    pos = (0.0, 0.0)
    start = (0.0, 0.0)
    prev_cubic = None
    prev_quad = None
    cmd = None
    i = 0

    def flush():
        nonlocal cur
        if len(cur) >= 1:
            polys.append(cur)
        cur = []

    while i < len(toks):
        if isinstance(toks[i], str):
            cmd = toks[i]
            i += 1
            if cmd in 'Zz':
                if cur:
                    cur.append(start)
                flush()
                pos = start
                continue
        rel = cmd.islower()
        c = cmd.upper()

        def take(n):
            nonlocal i
            vals = toks[i:i + n]
            i += n
            return vals

        if c == 'M':
            x, y = take(2)
            pos = (pos[0] + x, pos[1] + y) if rel else (x, y)
            flush()
            cur = [pos]
            start = pos
            # Пары чисел после M продолжают путь как L — так велит спецификация.
            cmd = 'l' if rel else 'L'
        elif c == 'L':
            x, y = take(2)
            pos = (pos[0] + x, pos[1] + y) if rel else (x, y)
            cur.append(pos)
        elif c == 'H':
            x = take(1)[0]
            pos = (pos[0] + x, pos[1]) if rel else (x, pos[1])
            cur.append(pos)
        elif c == 'V':
            y = take(1)[0]
            pos = (pos[0], pos[1] + y) if rel else (pos[0], y)
            cur.append(pos)
        elif c in ('C', 'S'):
            if c == 'C':
                x1, y1, x2, y2, x, y = take(6)
                p1 = (pos[0] + x1, pos[1] + y1) if rel else (x1, y1)
            else:
                x2, y2, x, y = take(4)
                p1 = (2 * pos[0] - prev_cubic[0], 2 * pos[1] - prev_cubic[1]) if prev_cubic else pos
            p2 = (pos[0] + x2, pos[1] + y2) if rel else (x2, y2)
            p3 = (pos[0] + x, pos[1] + y) if rel else (x, y)
            flatten_cubic(pos, p1, p2, p3, cur, steps)
            prev_cubic = p2
            pos = p3
        elif c in ('Q', 'T'):
            if c == 'Q':
                x1, y1, x, y = take(4)
                q = (pos[0] + x1, pos[1] + y1) if rel else (x1, y1)
            else:
                x, y = take(2)
                q = (2 * pos[0] - prev_quad[0], 2 * pos[1] - prev_quad[1]) if prev_quad else pos
            p3 = (pos[0] + x, pos[1] + y) if rel else (x, y)
            # Квадратичную поднимаем до кубической: один код сглаживания вместо двух.
            p1 = (pos[0] + 2.0 / 3.0 * (q[0] - pos[0]), pos[1] + 2.0 / 3.0 * (q[1] - pos[1]))
            p2 = (p3[0] + 2.0 / 3.0 * (q[0] - p3[0]), p3[1] + 2.0 / 3.0 * (q[1] - p3[1]))
            flatten_cubic(pos, p1, p2, p3, cur, steps)
            prev_quad = q
            pos = p3
        elif c == 'A':
            rx, ry, rot, large, sweep, x, y = take(7)
            p1 = (pos[0] + x, pos[1] + y) if rel else (x, y)
            flatten_arc(pos, rx, ry, rot, int(large), int(sweep), p1, cur, steps)
            pos = p1
        else:
            raise ValueError('неизвестная команда пути: %r' % cmd)

        if c not in ('C', 'S'):
            prev_cubic = None
        if c not in ('Q', 'T'):
            prev_quad = None

    flush()
    return polys


PATH_RE = re.compile(r'<path\b([^>]*)>', re.S)
D_RE = re.compile(r'\bd="([^"]*)"')
SW_RE = re.compile(r'\bstroke-width="([^"]*)"')


def read_svg(path):
    """Ломаные значка и толщина обводки в единицах viewBox 24x24."""
    text = open(path, encoding='utf-8').read()
    vb = re.search(r'viewBox="([^"]+)"', text)
    box = [float(v) for v in vb.group(1).split()] if vb else [0, 0, 24, 24]
    doc_w = re.search(r'\bstroke-width="([^"]*)"', text)
    default_sw = float(doc_w.group(1)) if doc_w else 2.0

    strokes = []
    for m in PATH_RE.finditer(text):
        attrs = m.group(1)
        # Первый путь Tabler — прозрачная рамка 24x24 со stroke="none". Она
        # задаёт габарит и рисоваться не должна.
        if re.search(r'\bstroke="none"', attrs):
            continue
        dm = D_RE.search(attrs)
        if not dm:
            continue
        sw = SW_RE.search(attrs)
        width = float(sw.group(1)) if sw else default_sw
        for poly in parse_path(dm.group(1)):
            if len(poly) >= 1:
                strokes.append((poly, width))
    return strokes, box


# ---------------------------------------------------------------------------
#  Растеризация
# ---------------------------------------------------------------------------

def raster(strokes, box, size):
    """Покрытие size x size, байт на пиксель.

    Обводка круглым пером — это множество точек не дальше половины толщины от
    ломаной. Поэтому считаем расстояние до ломаной и переводим его в покрытие:
    круглые концы и стыки получаются сами, без единого особого случая.
    """
    bx, by, bw, bh = box
    scale = size / max(bw, bh)
    px = [0] * (size * size)

    # Отрезки и радиусы сразу в пикселях — иначе перевод пришлось бы делать
    # внутри двойного цикла по пикселям.
    segs = []
    for poly, width in strokes:
        r = max(0.35, width * 0.5 * scale)
        pts = [((x - bx) * scale, (y - by) * scale) for (x, y) in poly]
        if len(pts) == 1:
            segs.append((pts[0], pts[0], r))
            continue
        for a, b in zip(pts, pts[1:]):
            segs.append((a, b, r))
    if not segs:
        return bytes(px)

    for y in range(size):
        cy = y + 0.5
        row = y * size
        for x in range(size):
            cx = x + 0.5
            best = 1e9
            for (ax, ay), (bx2, by2), r in segs:
                dx, dy = bx2 - ax, by2 - ay
                L2 = dx * dx + dy * dy
                if L2 <= 1e-12:
                    d = math.hypot(cx - ax, cy - ay) - r
                else:
                    t = ((cx - ax) * dx + (cy - ay) * dy) / L2
                    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
                    d = math.hypot(cx - ax - t * dx, cy - ay - t * dy) - r
                if d < best:
                    best = d
                    if best <= -0.75:
                        break
            # Полпикселя перехода: край получается мягким ровно на ширину
            # пикселя, а не размытым на два.
            cov = 0.5 - best
            if cov <= 0.0:
                continue
            px[row + x] = 255 if cov >= 1.0 else int(cov * 255.0 + 0.5)
    return bytes(px)


# ---------------------------------------------------------------------------
#  Атлас и генерация кода
# ---------------------------------------------------------------------------

def pack(images, size, pad=1):
    """Сетка одинаковых ячеек. Значки одного размера — паковать их деревом
    незачем: сетка не теряет ни пикселя и даёт координаты одной формулой."""
    n = len(images)
    cell = size + pad * 2
    cols = max(1, int(math.ceil(math.sqrt(n))))
    rows = int(math.ceil(n / cols))
    w, h = cols * cell, rows * cell
    atlas = bytearray(w * h)
    rects = []
    for i, img in enumerate(images):
        cx, cy = (i % cols) * cell + pad, (i // cols) * cell + pad
        for y in range(size):
            dst = (cy + y) * w + cx
            atlas[dst:dst + size] = img[y * size:(y + 1) * size]
        rects.append((cx, cy))
    return bytes(atlas), w, h, rects


def cpp_bytes(data, per_line=24):
    out = []
    for i in range(0, len(data), per_line):
        out.append('    ' + ' '.join('0x%02X,' % b for b in data[i:i + per_line]))
    return '\n'.join(out)


def main():
    check_only = '--check' in sys.argv
    manifest = json.load(open(MANIFEST, encoding='utf-8'))
    src_dir = os.path.join(REPO, manifest['source'])
    sizes = manifest['sizes']
    icons = manifest['icons']
    names = list(icons.keys())

    missing = [n for n in names if not os.path.exists(os.path.join(src_dir, icons[n] + '.svg'))]
    if missing:
        print('НЕТ ИСХОДНИКОВ (%d): %s' % (len(missing), ', '.join(missing[:8])))
        return 1

    # Разные имена могут указывать на один исходник (Copy и Duplicate) — рисуем
    # его один раз, а в атласе он и лежит один.
    per_size = {}
    for size in sizes:
        cache = {}
        images = []
        for name in names:
            tab = icons[name]
            if tab not in cache:
                strokes, box = read_svg(os.path.join(src_dir, tab + '.svg'))
                cache[tab] = raster(strokes, box, size)
            images.append(cache[tab])
        per_size[size] = images
        print('  %2d px: значков %d, уникальных рисунков %d' % (size, len(images), len(cache)))

    atlases = []
    for size in sizes:
        atlas, w, h, rects = pack(per_size[size], size)
        atlases.append((size, atlas, w, h, rects))
        print('  атлас %2d px: %dx%d, %d КиБ' % (size, w, h, len(atlas) // 1024))

    header = ['// СГЕНЕРИРОВАНО sage_icon_builder.py — НЕ ПРАВИТЬ РУКАМИ.',
              '// Источник: engine/icons/icons.json + engine/icons/tabler/*.svg',
              '// Пересобрать: python3 scripts/sage_icon_builder.py',
              '#pragma once',
              '#include <cstdint>',
              '',
              'namespace sage::ui::icons {',
              '',
              '// Значки перечислением, а не строкой: опечатка в имени обязана быть',
              '// ошибкой компиляции, а поиск значка — сложением адреса, а не поиском',
              '// по таблице строк (§9 ТЗ).',
              'enum class Icon : uint16_t {']
    for name in names:
        header.append('    %s,' % name)
    header += ['    Count,',
               '};',
               '',
               'inline constexpr int kIconCount = %d;' % len(names),
               'inline constexpr int kAtlasCount = %d;' % len(sizes),
               'inline constexpr int kAtlasSizes[kAtlasCount] = {%s};'
               % ', '.join(str(s) for s in sizes),
               '',
               '} // namespace sage::ui::icons']

    source = ['// СГЕНЕРИРОВАНО sage_icon_builder.py — НЕ ПРАВИТЬ РУКАМИ.',
              '#include "sage/ui/icons/SageIcons.generated.h"',
              '',
              '#include "sage/ui/icons/SageIcons.h"',
              '',
              'namespace sage::ui::icons {',
              'namespace generated {',
              '']
    for idx, (size, atlas, w, h, rects) in enumerate(atlases):
        source.append('// Атлас %d px: %dx%d, покрытие R8 — как у атласа шрифта.' % (size, w, h))
        source.append('const unsigned char kAtlas%d[] = {' % size)
        source.append(cpp_bytes(atlas))
        source.append('};')
        source.append('')
    source.append('const IconAtlasData kAtlases[] = {')
    for size, atlas, w, h, rects in atlases:
        source.append('    {%d, %d, %d, kAtlas%d, sizeof(kAtlas%d)},' % (size, w, h, size, size))
    source.append('};')
    source.append('')
    source.append('// Имя -> номер. Строки нужны плагинам и редактору документов; сам')
    source.append('// интерфейс обращается по enum и в эту таблицу не заглядывает.')
    source.append('const char* const kNames[] = {')
    for name in names:
        source.append('    "%s",' % name)
    source.append('};')
    source.append('')
    source.append('// Ячейка значка в каждом атласе: левый верхний угол в пикселях.')
    source.append('const IconCell kCells[] = {')
    for i, name in enumerate(names):
        cells = ', '.join('{%d, %d}' % atlases[a][4][i] for a in range(len(atlases)))
        source.append('    {{%s}}, // %s' % (cells, name))
    source.append('};')
    source += ['', '} // namespace generated', '} // namespace sage::ui::icons']

    header_text = '\n'.join(header) + '\n'
    source_text = '\n'.join(source) + '\n'

    if check_only:
        ok = True
        for path, text in ((OUT_HEADER, header_text), (OUT_SOURCE, source_text)):
            have = open(path, encoding='utf-8').read() if os.path.exists(path) else None
            if have != text:
                print('УСТАРЕЛ: %s' % os.path.relpath(path, REPO))
                ok = False
        if ok:
            print('значки: сгенерированное соответствует манифесту (%d значков)' % len(names))
        else:
            print('    Лечится: python3 scripts/sage_icon_builder.py')
        return 0 if ok else 1

    os.makedirs(OUT_DIR, exist_ok=True)
    open(OUT_HEADER, 'w', encoding='utf-8').write(header_text)
    open(OUT_SOURCE, 'w', encoding='utf-8').write(source_text)
    total = sum(len(a[1]) for a in atlases)
    print('готово: %d значков, %d атласов, %d КиБ покрытия' % (len(names), len(atlases), total // 1024))
    print('  %s' % os.path.relpath(OUT_HEADER, REPO))
    print('  %s' % os.path.relpath(OUT_SOURCE, REPO))
    return 0


if __name__ == '__main__':
    sys.exit(main())

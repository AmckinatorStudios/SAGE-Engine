#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Настоящая системная мышь X11 (расширение XTest) против захвата курсора игры.

ЗАЧЕМ. Пробы редактора подают положения курсора прямо в окно (EmitCursorPos) —
они проверяют логику движка, но не то, что приходит от СИСТЕМЫ: сырой ввод,
захват указателя, фокус. Здесь мышь двигает X-сервер, как её двигал бы
человек, а игра (камера от первого лица, просит захват каждый кадр) пишет в
лог накопленный поворот. Проверяется:
  • долгий поворот доходит до камеры целиком — без потерь и скачков;
  • пока окно без фокуса, движение мыши в другом окне камеру не двигает;
  • после возврата фокуса поворот продолжается с того же места.

Запуск (из ci_smoke_test.sh, под xvfb):  xtest_mouse.py <лог игры>
Код возврата 0 — всё сошлось, 1 — нет, 2 — нет libXtst (проверка пропущена).
"""
import ctypes
import re
import sys
import time

try:
    x11 = ctypes.CDLL('libX11.so.6')
    xt = ctypes.CDLL('libXtst.so.6')
except OSError:
    print('нет libX11/libXtst — проверка мыши пропущена')
    sys.exit(2)

x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XDefaultRootWindow.restype = ctypes.c_ulong
x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x11.XCreateSimpleWindow.restype = ctypes.c_ulong
x11.XCreateSimpleWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                    ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_ulong]

LOG = sys.argv[1]
d = x11.XOpenDisplay(None)
if not d:
    print('нет дисплея X')
    sys.exit(1)
D = ctypes.c_void_p(d)
root = x11.XDefaultRootWindow(d)


def windows():
    r, p, n = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_uint()
    ch = ctypes.POINTER(ctypes.c_ulong)()
    x11.XQueryTree(D, ctypes.c_ulong(root), ctypes.byref(r), ctypes.byref(p), ctypes.byref(ch), ctypes.byref(n))
    return [ch[i] for i in range(n.value)]


def has_name(w):
    s = ctypes.c_char_p()
    x11.XFetchName(D, ctypes.c_ulong(w), ctypes.byref(s))
    return bool(s.value)


def focus(w):
    x11.XSetInputFocus(D, ctypes.c_ulong(w), 1, 0)   # RevertToPointerRoot, CurrentTime
    x11.XSync(D, 0)


def move(dx, steps, pause=0.01):
    for _ in range(steps):
        xt.XTestFakeRelativeMotionEvent(D, ctypes.c_int(dx), ctypes.c_int(0), ctypes.c_ulong(0))
        x11.XFlush(D)
        time.sleep(pause)


def last_yaw():
    with open(LOG, encoding='utf-8', errors='replace') as f:
        found = re.findall(r'LOOK: yaw=(-?[\d.]+)', f.read())
    return float(found[-1]) if found else 0.0


def yaw():
    """Поворот камеры, когда он перестал меняться: игра под нагрузкой рисует
    кадры реже, и движение мыши доходит до неё не сразу."""
    value, still_since, deadline = last_yaw(), time.time(), time.time() + 10.0
    while time.time() < deadline:
        time.sleep(0.2)
        now = last_yaw()
        if now != value:
            value, still_since = now, time.time()
        elif time.time() - still_since >= 1.0:
            break
    return value


game = None
for _ in range(300):
    named = [w for w in windows() if has_name(w)]
    if named:
        game = named[-1]
        break
    time.sleep(0.1)
if not game:
    print('окно игры не появилось')
    sys.exit(1)

time.sleep(2.0)
focus(game)                      # «щелчок» по окну: фокус у игры
time.sleep(1.0)
start = yaw()
move(5, 600)                     # длинный поворот: 600 x 5 = 3000 пикселей
time.sleep(1.0)
turned = yaw() - start

other = x11.XCreateSimpleWindow(d, root, 0, 0, 200, 200, 0, 0, 0)
x11.XMapWindow(D, ctypes.c_ulong(other))
x11.XSync(D, 0)
time.sleep(0.3)
focus(other)                     # alt-tab: фокус у другого окна
time.sleep(0.5)
before = yaw()
move(7, 100)                     # мышь двигают вне игры
time.sleep(1.0)
idle = yaw() - before

focus(game)                      # вернулись в игру
time.sleep(1.0)
before = yaw()
move(5, 200)                     # +1000
time.sleep(1.0)
resumed = yaw() - before

with open(LOG, encoding='utf-8', errors='replace') as f:
    jumps = f.read().count('LOOK: JUMP')
print(f'поворот {turned:.0f} из 3000, без фокуса {idle:.0f}, после возврата {resumed:.0f} из 1000, '
      f'скачков {jumps}')
ok = 2990 <= turned <= 3005 and abs(idle) <= 10 and 990 <= resumed <= 1005 and jumps == 0
sys.exit(0 if ok else 1)

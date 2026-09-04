#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Движок — единая платформа, а не набор библиотек, которые игра связывает сама.

ЧТО ПРОВЕРЯЕТСЯ. Код игры (games/) не включает сторонние библиотеки движка —
GLFW, glm, entt, ImGui, nlohmann, sol2 — и не зовёт их функции напрямую. Всё,
что игре нужно, движок отдаёт своим API под <sage/...>.

ПОЧЕМУ ЭТО ВАЖНО, А НЕ ПРОСТО КРАСИВО. Игра, которая пишет
`InputBinding::Key(GLFW_KEY_W)`, обязана знать, что внутри движка стоит GLFW,
подключить его заголовок и жить с его константами. Из этого следуют три вещи,
и ни одна не косметическая:

  • движок перестаёт быть платформой: разработчик игры связывает библиотеки
    сам, хотя просил у движка всего лишь ввод;
  • смена оконной библиотеки или появление второго бэкенда ломает КАЖДУЮ игру,
    а не одну реализацию внутри движка;
  • автор игры читает документацию GLFW там, где рассчитывал обойтись
    документацией SAGE.

Ровно так и было: коды клавиш, захват курсора и время кадра игры брали у GLFW.
Теперь для этого есть sage::Key, Window::SetCursorCaptured и Application::Time,
а проверка сторожит, чтобы протечка не вернулась.

ЧЕГО ПРОВЕРКА НЕ ЗАПРЕЩАЕТ. Использовать сторонние библиотеки ВНУТРИ движка
(engine/, editor/, runtime/) — это его дело и его свобода. Запрет касается
только границы «движок отдаёт игре».

Запуск:  python3 scripts/check_engine_api.py
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES_DIR = os.path.join(REPO, 'games')

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')

# Сторонние библиотеки, которых игровой код видеть не должен.
FORBIDDEN_INCLUDES = (
    'GLFW/',
    'glm/',
    'entt/',
    'imgui',
    'nlohmann/',
    'sol/',
    'glad/',
    'stb_',
)

# Прямые вызовы и константы — на случай, если заголовок пришёл транзитивно.
FORBIDDEN_TOKENS = re.compile(r'\b(glfw[A-Z]\w*|GLFW_[A-Z0-9_]+|ImGui::)')

# Строки, где упоминание допустимо: комментарий, объясняющий, ПОЧЕМУ так больше
# не делают, — самая полезная документация из возможных, и запрещать её значит
# стирать историю решения.
def is_comment(line):
    stripped = line.lstrip()
    return stripped.startswith('//') or stripped.startswith('*') or stripped.startswith('/*')


def sources():
    for dirpath, _dirnames, filenames in os.walk(GAMES_DIR):
        for name in filenames:
            if name.endswith(('.h', '.hpp', '.c', '.cpp', '.cc')):
                yield os.path.join(dirpath, name)


def main():
    if not os.path.isdir(GAMES_DIR):
        print('games/ нет — проверять нечего')
        return 0

    problems = []
    checked = 0
    for path in sources():
        checked += 1
        rel = os.path.relpath(path, REPO)
        with open(path, encoding='utf-8') as handle:
            for lineno, line in enumerate(handle, 1):
                if is_comment(line):
                    continue

                match = INCLUDE_RE.match(line)
                if match and match.group(1).startswith(FORBIDDEN_INCLUDES):
                    problems.append(
                        '%s:%d: игра включает стороннюю библиотеку <%s>'
                        % (rel, lineno, match.group(1)))
                    continue

                found = FORBIDDEN_TOKENS.search(line)
                if found:
                    problems.append(
                        '%s:%d: игра зовёт %s напрямую' % (rel, lineno, found.group(1)))

    if problems:
        print('Движок протекает в игровой код:')
        for problem in problems:
            print('  ' + problem)
        print()
        print('Игре положено обращаться к движку, а не к его внутренностям:')
        print('  коды клавиш      -> sage::Key / sage::MouseButton   (sage/core/Keys.h)')
        print('  захват курсора   -> Window::SetCursorCaptured       (sage/core/Window.h)')
        print('  время            -> Application::Time               (sage/core/Application.h)')
        print('  математика       -> sage::Vec3 и прочее             (sage/core/Math.h)')
        print('  всё сразу        -> #include <sage/Sage.h>')
        return 1

    print('Граница «движок -> игра» держится: просмотрено файлов %d, '
          'сторонних библиотек в играх нет.' % checked)
    return 0


if __name__ == '__main__':
    sys.exit(main())

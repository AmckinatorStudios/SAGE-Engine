#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Подсказка по скриптовому API для ЛЮБОГО редактора кода.

ЗАЧЕМ. Скрипты пишут не в SAGE — своего редактора кода у движка нет намеренно
(см. editor/src/CodeEditorApp.h). Пишут их в VS Code, Neovim, JetBrains, Zed. И
до сих пор там не было ни одной подсказки: движок даёт под три сотни функций в
двух с половиной десятках модулей, а редактор про них не знает ничего — ни
имени, ни аргументов. Единственным способом узнать, как называется функция и
что она берёт, было чтение исходников движка на C++.

ЧТО ЭТО ЗА ФАЙЛ. Описание API на языке аннотаций LuaLS/EmmyLua — его понимают
ВСЕ распространённые редакторы, потому что за подсказками к Lua в каждом из них
стоит один и тот же lua-language-server (VS Code «Lua» от sumneko, Neovim,
Zed, Helix) либо совместимый с ним EmmyLua (JetBrains). Это не «файл для VS
Code», а общий формат — ровно поэтому его и выбрали.

ПОЧЕМУ ГЕНЕРАТОР, А НЕ ФАЙЛ РУКАМИ. Подсказка, написанная руками, расходится с
движком на первой же правке — и начинает ВРАТЬ: предлагает функцию, которой
нет, и молчит о той, что появилась. Врущая подсказка хуже отсутствующей:
отсутствующую человек компенсирует документацией, а врущей он верит. Поэтому
источник правды один — вызовы Bind(...) в engine/src/sage/scripting/, то есть
ровно то, что движок регистрирует на самом деле.

Свежесть файла сторожит scripts/gen_script_api.py --check: разошёлся — CI падает.

Запуск:
    python3 scripts/gen_script_api.py            # переписать editor/assets/api/sage.lua
    python3 scripts/gen_script_api.py --check    # только сверить, ничего не писать
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = sorted((ROOT / "engine/src/sage/scripting").glob("*.cpp"))
OUT = ROOT / "editor/assets/api/sage.lua"

# Bind("модуль", "Имя", "СтароеИмя"|nullptr, [захват](аргументы) -> тип {
BIND = re.compile(
    r'\bBind\(\s*"([a-z_]+)"\s*,\s*"(\w+)"\s*,\s*(nullptr|"[\w]+")\s*,\s*'
    r'\[[^\]]*\]\s*\(([^)]*)\)\s*(?:->\s*([^{]+?)\s*)?\{',
    re.S)

# C++ -> Lua. Неизвестное осознанно становится any: соврать типом хуже, чем
# промолчать о нём — подсказка с неверным типом уводит в сторону молча.
TYPES = [
    (r'\bglm::vec2\b', 'Vec2'), (r'\bglm::vec3\b', 'Vec3'), (r'\bglm::vec4\b', 'Vec4'),
    (r'\bglm::quat\b', 'Vec4'),
    (r'\bGameObject\b', 'Entity'),
    (r'\bstd::string\b', 'string'),
    (r'\bbool\b', 'boolean'),
    (r'\b(?:float|double|int|unsigned|size_t|int64_t|uint32_t|long long)\b', 'number'),
    (r'\bsol::table\b', 'table'),
    (r'\bsol::(?:protected_function|function)\b', 'function'),
    (r'\bsol::(?:object|variadic_args|nil_t)\b', 'any'),
    (r'\bvoid\b', 'nil'),
]


def lua_type(cpp):
    """Тип аргумента или возврата. Второе значение — необязательный ли он."""
    text = cpp.strip()
    optional = False
    m = re.search(r'sol::optional\s*<\s*(.+?)\s*>\s*$', text)
    if m:
        optional = True
        text = m.group(1)
    text = re.sub(r'\b(const|&|\*)\b', ' ', text).replace('&', ' ').replace('*', ' ')
    for pattern, name in TYPES:
        if re.search(pattern, text):
            return name, optional
    return 'any', optional


def split_args(text):
    """Аргументы лямбды: по запятым нулевого уровня (в типах есть свои)."""
    out, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        elif ch == ',' and depth == 0:
            out.append(text[start:i])
            start = i + 1
    out.append(text[start:])
    return [a for a in (x.strip() for x in out) if a]


def arg_name(cpp, index):
    """Имя аргумента из объявления; безымянному даём номер."""
    m = re.search(r'(\w+)\s*$', cpp.strip())
    name = m.group(1) if m else ''
    if not name or name in ('float', 'int', 'bool', 'double', 'unsigned', 'string',
                            'GameObject', 'table', 'object', 'size_t'):
        return f'arg{index + 1}'
    # `self` и `obj` у методов сущности читаются понятнее как entity
    return 'entity' if name in ('obj', 'object') else name


def collect():
    """{модуль: [(имя, [(имя_арг, тип, необязателен)], тип_возврата, старое_имя)]}"""
    modules = {}
    legacy = []
    for path in SOURCES:
        text = path.read_text(encoding='utf-8')
        for m in BIND.finditer(text):
            module, name, old, args, ret = m.groups()
            params = []
            for i, a in enumerate(split_args(args)):
                t, opt = lua_type(a)
                params.append((arg_name(a, i), t, opt))
            rtype = lua_type(ret)[0] if ret else 'nil'
            modules.setdefault(module, []).append((name, params, rtype))
            if old != 'nullptr':
                legacy.append((old.strip('"'), module, name))
    for fns in modules.values():
        fns.sort(key=lambda f: f[0])
    return modules, sorted(legacy)


HEADER = '''---@meta
-- ===========================================================================
--  ПОДСКАЗКА ПО СКРИПТОВОМУ API SAGE — для любого редактора кода.
--
--  ФАЙЛ СОБРАН АВТОМАТИЧЕСКИ из вызовов Bind(...) в engine/src/sage/scripting/.
--  Править руками бессмысленно: следующий запуск scripts/gen_script_api.py
--  перезапишет. Нужна другая подсказка — меняйте движок, она поедет следом.
--
--  КАК ПОДКЛЮЧИТЬ. В новом проекте уже подключено: рядом с ним лежат
--  .luarc.json и .sage/api/sage.lua, и редактор находит их сам. Чужому проекту
--  хватит .luarc.json со строкой
--      { "workspace.library": [".sage/api"] }
--
--  ДВА ИМЕНИ У КАЖДОЙ ФУНКЦИИ, и оба настоящие:
--      sage.game.Quit()   -- полное: видно, чьё оно, и его не затрёт своя функция
--      game.Quit()        -- короткое: тот же самый объект, а не копия
--  Короткое имя — это ПСЕВДОНИМ модуля в глобальных, и движок ставит его,
--  только если имя свободно: игра, объявившая свою `scene`, остаётся при своей.
-- ===========================================================================

---@class Vec2
---@field x number
---@field y number
Vec2 = {}
---@param x number
---@param y number
---@return Vec2
function Vec2.new(x, y) end

---@class Vec3
---@field x number
---@field y number
---@field z number
Vec3 = {}
---@param x number
---@param y number
---@param z number
---@return Vec3
function Vec3.new(x, y, z) end

---@class Vec4
---@field x number
---@field y number
---@field z number
---@field w number
Vec4 = {}
---@param x number
---@param y number
---@param z number
---@param w number
---@return Vec4
function Vec4.new(x, y, z, w) end

---@class Transform
---@field Position Vec3
---@field Rotation Vec3
---@field Scale Vec3

---Сущность сцены: то, что приходит в OnStart/OnUpdate первым аргументом.
---@class Entity
---@field Name string
---@field Transform Transform
---@field Color Vec3

---СВОЯ сущность — та, к которой привязан этот скрипт. Видна во всём файле:
---и в коде верхнего уровня, и в функциях, вынесенных из хуков. Внутри хука это
---тот же объект, что и его аргумент: `self == entity`.
---@type Entity
self = nil

---Пишет строку в консоль редактора и в лог игры.
---@param message any
function log(message) end

---Ждать внутри StartCoroutine. Вне корутины смысла не имеет.
---@param seconds number
function wait(seconds) end

'''

HOOKS = '''
-- --- Хуки скрипта ---------------------------------------------------------
-- Объявляются в файле скрипта; движок зовёт их сам. Ни один не обязателен.

---Один раз при старте.
---@param entity Entity
function OnStart(entity) end

---Каждый кадр. dt — секунд с прошлого кадра.
---@param entity Entity
---@param dt number
function OnUpdate(entity, dt) end

---Сообщение от другого скрипта (sage.msg.Send/Broadcast).
---@param entity Entity
---@param name string
---@param data any
function OnMessage(entity, name, data) end
'''


def render(modules, legacy):
    out = [HEADER]
    out.append('-- --- Модули ---------------------------------------------------------------\n')
    out.append('---@class sage\n')
    for module in sorted(modules):
        out.append(f'---@field {module} sage.{module}\n')
    out.append('sage = {}\n\n')

    for module in sorted(modules):
        out.append(f'---@class sage.{module}\n')
        out.append(f'sage.{module} = {{}}\n\n')
        out.append(f'---Короткое имя того же модуля: `{module}.X()` — это ТОТ ЖЕ объект,\n')
        out.append(f'---что `sage.{module}`, а не копия. Движок ставит его, только если имя\n')
        out.append('---свободно: игра, объявившая своё, остаётся при своём.\n')
        out.append(f'---@type sage.{module}\n{module} = nil\n\n')
        for name, params, rtype in modules[module]:
            for pname, ptype, opt in params:
                out.append(f'---@param {pname}{"?" if opt else ""} {ptype}\n')
            if rtype != 'nil':
                out.append(f'---@return {rtype}\n')
            arglist = ', '.join(p[0] for p in params)
            out.append(f'function sage.{module}.{name}({arglist}) end\n')
        out.append('\n')

    out.append('-- --- Старые глобальные имена ----------------------------------------------\n')
    out.append('-- Те же функции, что выше, под именами, с которых начинался движок.\n')
    out.append('-- Работают и работать будут: на них написаны существующие игры.\n')
    for old, module, name in legacy:
        out.append(f'---@type fun(...): any\n{old} = sage.{module}.{name}\n')
    out.append(HOOKS)
    return ''.join(out)


def main():
    modules, legacy = collect()
    text = render(modules, legacy)
    check = '--check' in sys.argv
    total = sum(len(v) for v in modules.values())
    if check:
        if not OUT.exists() or OUT.read_text(encoding='utf-8') != text:
            print(f"Подсказка {OUT.relative_to(ROOT)} устарела: движок регистрирует "
                  f"{total} функций в {len(modules)} модулях, а в файле лежит другое.\n"
                  f"Перезапишите её: python3 scripts/gen_script_api.py")
            return 1
        print(f"Подсказка по API свежая: функций {total}, модулей {len(modules)}.")
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding='utf-8')
    print(f"{OUT.relative_to(ROOT)}: функций {total}, модулей {len(modules)}, "
          f"старых имён {len(legacy)}")
    return 0


if __name__ == '__main__':
    sys.exit(main())

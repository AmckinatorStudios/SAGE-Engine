#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Python как язык скриптов SAGE — эталонный адаптер протокола внешних языков.

Подключение к проекту (scripting/languages.json):

    {"languages": [{"id": "python", "name": "Python", "extensions": [".py"],
                    "command": ["python3", "{project}/scripting/sage_python.py"]}]}

Скрипт объекта — обычный файл Python:

    speed = 5.0                    # публичное поле: видно в инспекторе

    def start(self):
        self.hp = 10

    def update(self, dt):
        pos = sage.object.position(self.entity)   # любая функция API движка
        ...

    def take_damage(self, n):      # метод: его зовут другие скрипты на любом языке
        self.hp -= n
        return self.hp

Хуки — имена движка в snake_case (start, update, fixed_update, late_update,
on_enable, on_disable, on_destroy, on_collision_enter, …). Поля — присваивания
литералов на верхнем уровне файла; они разбираются БЕЗ выполнения файла.
API движка — объект `sage`: `sage.math.Clamp(5, 0, 2)` или
`sage.call("sage.math.Clamp", 5, 0, 2)`; типы значений — Vec2/Vec3/Vec4/Entity
отсюда, числа, строки, списки. Протокол — в sage/scripting/ProcessLanguage.h.
"""
import ast
import json
import re
import sys
import traceback

_out = sys.stdout
# print() скрипта не должен ломать протокол: обычный вывод идёт в журнал движка.
sys.stdout = sys.stderr


class Vec2(tuple):
    def __new__(cls, x=0.0, y=0.0):
        return tuple.__new__(cls, (float(x), float(y)))
    x = property(lambda s: s[0]); y = property(lambda s: s[1])


class Vec3(tuple):
    def __new__(cls, x=0.0, y=0.0, z=0.0):
        return tuple.__new__(cls, (float(x), float(y), float(z)))
    x = property(lambda s: s[0]); y = property(lambda s: s[1]); z = property(lambda s: s[2])


class Vec4(tuple):
    def __new__(cls, x=0.0, y=0.0, z=0.0, w=0.0):
        return tuple.__new__(cls, (float(x), float(y), float(z), float(w)))
    x = property(lambda s: s[0]); y = property(lambda s: s[1])
    z = property(lambda s: s[2]); w = property(lambda s: s[3])


class Entity:
    def __init__(self, id):
        self.id = int(id)

    def __eq__(self, other):
        return isinstance(other, Entity) and other.id == self.id

    def __repr__(self):
        return "Entity(%d)" % self.id


def to_value(v):
    if isinstance(v, bool):
        return {"kind": "bool", "value": v}
    if isinstance(v, int):
        return {"kind": "int", "value": v}
    if isinstance(v, float):
        return {"kind": "float", "value": v}
    if isinstance(v, str):
        return {"kind": "string", "value": v}
    if isinstance(v, Vec2):
        return {"kind": "vec2", "value": {"x": v.x, "y": v.y}}
    if isinstance(v, Vec3):
        return {"kind": "vec3", "value": {"x": v.x, "y": v.y, "z": v.z}}
    if isinstance(v, Vec4):
        return {"kind": "color", "value": {"x": v.x, "y": v.y, "z": v.z, "w": v.w}}
    if isinstance(v, Entity):
        return {"kind": "entity", "value": v.id}
    if isinstance(v, (list, tuple)):
        return {"kind": "array", "value": [to_value(x) for x in v]}
    if v is None:
        return {"kind": "bool", "value": False}
    raise TypeError("значение %r нельзя передать движку" % (v,))


def from_value(j):
    kind, v = j.get("kind"), j.get("value")
    if kind in ("bool", "int", "float", "string", "asset"):
        return v
    if kind == "vec2":
        return Vec2(v["x"], v["y"])
    if kind == "vec3":
        return Vec3(v["x"], v["y"], v["z"])
    if kind in ("color", "rotation"):
        return Vec4(v["x"], v["y"], v["z"], v["w"])
    if kind == "entity":
        return Entity(v)
    if kind == "array":
        return [from_value(x) for x in v]
    return v


def send(msg):
    _out.write(json.dumps(msg, ensure_ascii=False) + "\n")
    _out.flush()


def receive():
    line = sys.stdin.readline()
    if not line:
        sys.exit(0)
    return json.loads(line)


class Api:
    """sage.<модуль>.<Функция>(...) — вызов функции движка по каталогу API."""

    def __init__(self, path="sage"):
        self._path = path

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        return Api(self._path + "." + name)

    def __call__(self, *args):
        return call(self._path, *args)

    @staticmethod
    def call(name, *args):
        return call(name, *args)

    @staticmethod
    def log(*parts):
        send({"log": " ".join(str(p) for p in parts)})


def call(name, *args):
    send({"api": name, "args": [to_value(a) for a in args]})
    reply = receive()
    if "error" in reply:
        raise RuntimeError(reply["error"])
    results = [from_value(r) for r in reply.get("results", [])]
    return results[0] if len(results) == 1 else (tuple(results) if results else None)


def snake(name):
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


HOOKS = {snake(h) for h in ("Start", "Update", "FixedUpdate", "LateUpdate", "OnEnable", "OnDisable",
                            "OnDestroy", "OnCollisionEnter", "OnCollisionExit", "OnTriggerEnter",
                            "OnTriggerExit", "OnTriggerStay", "OnAnimationEvent", "OnKeyDown", "OnKeyUp")}


def public_fields(source, path):
    """Поля — присваивания литералов на верхнем уровне. Файл НЕ выполняется."""
    fields = {}
    for node in ast.parse(source, path).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            name = node.targets[0].id
            if name.startswith("_"):
                continue
            try:
                fields[name] = to_value(ast.literal_eval(node.value))
            except (ValueError, TypeError):
                pass
    return fields


class Self:
    def __init__(self, entity):
        self.entity = Entity(entity)


instances = {}


def error_reply(exc, path):
    line = 0
    if isinstance(exc, SyntaxError):
        line = exc.lineno or 0
    else:
        for frame in traceback.extract_tb(exc.__traceback__):
            if frame.filename == path:
                line = frame.lineno
    return {"ok": False, "error": "%s: %s" % (type(exc).__name__, exc), "line": line}


def handle(req):
    op = req.get("op")
    if op == "hello":
        return {"ok": True, "language": "Python", "version": sys.version.split()[0]}
    if op == "fields":
        return {"ok": True, "fields": public_fields(req["source"], req["path"])}
    if op == "create":
        path = req["path"]
        env = {"sage": Api(), "Vec2": Vec2, "Vec3": Vec3, "Vec4": Vec4, "Entity": Entity, "__name__": path}
        exec(compile(req["source"], path, "exec"), env)
        me = Self(req.get("entity", 0))
        for name, value in public_fields(req["source"], path).items():
            setattr(me, name, from_value(value))
        funcs = {k: v for k, v in env.items() if callable(v) and getattr(v, "__module__", None) == path}
        instances[req["id"]] = (env, me, funcs, path)
        hooks = [h for h in ("Start", "Update", "FixedUpdate", "LateUpdate", "OnEnable", "OnDisable",
                             "OnDestroy", "OnCollisionEnter", "OnCollisionExit", "OnTriggerEnter",
                             "OnTriggerExit", "OnTriggerStay", "OnAnimationEvent", "OnKeyDown", "OnKeyUp")
                 if snake(h) in funcs]
        methods = [k for k in funcs if k not in HOOKS and not k.startswith("_")]
        return {"ok": True, "hooks": hooks, "methods": methods}
    if op == "set_fields":
        env, me, funcs, path = instances[req["id"]]
        for name, value in req.get("fields", {}).items():
            setattr(me, name, from_value(value))
        return {"ok": True}
    if op == "call":
        env, me, funcs, path = instances[req["id"]]
        fn = funcs.get(snake(req["hook"]))
        if fn:
            if "dt" in req:
                fn(me, req["dt"]) if fn.__code__.co_argcount > 1 else fn(me)
            elif "other" in req:
                fn(me, Entity(req["other"]))
            elif "name" in req:
                fn(me, req["name"])
            else:
                fn(me)
        return {"ok": True}
    if op == "invoke":
        env, me, funcs, path = instances[req["id"]]
        fn = funcs.get(req["method"])
        if not fn:
            return {"ok": False, "error": "нет метода «%s»" % req["method"]}
        result = fn(me, *[from_value(a) for a in req.get("args", [])])
        if result is None:
            return {"ok": True, "results": []}
        return {"ok": True, "results": [to_value(r) for r in (result if isinstance(result, tuple)
                                                              and not isinstance(result, (Vec2, Vec3, Vec4))
                                                              else (result,))]}
    if op == "destroy":
        instances.pop(req["id"], None)
        return {"ok": True}
    if op == "reset":
        instances.clear()
        return {"ok": True}
    return {"ok": False, "error": "неизвестная операция %r" % op}


def main():
    while True:
        req = receive()
        path = req.get("path") or (instances.get(req.get("id"), (None, None, None, ""))[3])
        try:
            send(handle(req))
        except Exception as exc:  # ошибка скрипта — ответ движку, а не смерть процесса
            send(error_reply(exc, path))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Компонент сцены описывается ОДИН раз — в реестре компонентов
(sage::scene::RegisterComponent, sage/scene/ComponentRegistry.h; встроенные —
sage/scene/BuiltinComponents.cpp), а не ветками в сериализаторе и копировании.

ЗАЧЕМ ЭТА ПРОВЕРКА СУЩЕСТВУЕТ. Пока каждый компонент был веткой в
SceneSerializer.cpp (запись и чтение) и строкой в Prefab.cpp (копия), новый
компонент требовал правки трёх мест, и списки расходились: копия объекта
(Ctrl+D, префаб, вставка) теряла тег, звук и твины, а повреждённые данные
одного компонента не давали открыть сцену вовсе. Реестр делает все три
действия по одному описанию и переживает чужие и битые данные.

Правило: в файлах ниже упоминаются только типы из ALLOWED — части самого
объекта (имя, положение, иерархия, меш) и ещё не переведённые в реестр. Новый
тип там — ошибка: его место в реестре. Список ALLOWED может только
сокращаться: тип, который из файлов ушёл, проверка требует из списка убрать,
чтобы он не вернулся молча.

Запуск: python3 scripts/check_components.py
"""
import os
import re
import sys

FILES = [
    "engine/src/sage/scene/SceneSerializer.cpp",
    "engine/src/sage/scene/Prefab.cpp",
]

# Тип -> почему он здесь, а не в реестре.
ALLOWED = {
    "IdComponent": "часть объекта: id",
    "NameComponent": "часть объекта: имя",
    "HierarchyComponent": "часть объекта: родитель и порядок",
    "Transform": "часть объекта: положение",
    "MeshRendererComponent": "часть объекта: меш и материалы (ключ noMesh, слоты)",
    "HiddenComponent": "метка без полей (флаг hidden)",
    "KeepOnLoadComponent": "метка без полей (флаг keepOnLoad)",
    "AnimationComponent": "только чтение: анимированному объекту статический меш не грузится",
    "sage::ui::InterfaceInstanceTag": "только чтение: экземпляр интерфейса не пишется в сцену",
    "sage::ui::Element": "интерфейс — реестром частей (sage::ui::RegisterPart)",
    # Ещё не переведены: несколько ключей на компонент ("script"/"scripts"/
    # "scriptGroups"), ссылки на объекты, чтение после "ui".
    "ScriptComponent": "не переведён: несколько ключей файла",
    "VarsComponent": "не переведён: ссылки (sage::vars::VisitEntityRefs)",
    "sage::signals::SignalLinksComponent": "не переведён: дополняется при чтении ui",
}

# Обращение к компоненту по типу: reg.try_get<T>, CopyIfPresent<T>, copyMark(T{}).
USE = re.compile(
    r"\b(?:try_get|emplace_or_replace|emplace|all_of|any_of|none_of|get_or_emplace|"
    r"remove|erase|get|view|CopyIfPresent|CopyComponent)\s*<\s*([A-Za-z_][A-Za-z0-9_:]*)\s*>"
    r"|\bcopyMark\s*\(\s*([A-Za-z_][A-Za-z0-9_:]*)\s*\{\s*\}\s*\)"
)
# Параметры шаблонов и встроенные типы — не компоненты.
IGNORE = {"T", "M", "int", "float", "bool", "std::string", "nlohmann::json", "json"}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    errors = []
    seen = set()
    for rel in FILES:
        with open(os.path.join(root, rel), encoding="utf-8") as f:
            lines = strip_comments(f.read()).split("\n")
        for n, line in enumerate(lines, 1):
            for m in USE.finditer(line):
                name = m.group(1) or m.group(2)
                if name in IGNORE:
                    continue
                seen.add(name)
                if name not in ALLOWED:
                    errors.append(
                        f"{rel}:{n}: компонент {name} — веткой мимо реестра. Опишите его "
                        f"sage::scene::RegisterComponent (встроенный — в "
                        f"sage/scene/BuiltinComponents.cpp): сохранение, чтение и копия — "
                        f"одним описанием."
                    )
    for name in sorted(set(ALLOWED) - seen):
        errors.append(
            f"scripts/check_components.py: {name} в файлах больше не встречается — "
            f"уберите его из ALLOWED, чтобы ветка не вернулась молча."
        )
    for e in errors:
        print(e)
    print(f"check_components: {len(errors)} нарушений, разрешено {len(ALLOWED)} типов")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

---
paths:
  - "engine/src/sage/scripting/**"
  - "editor/assets/api/**"
  - "tools/sage_api/**"
  - "tests/data/script_api_surface.txt"
  - "**/*.lua"
  - "**/languages.json"
  - "docs/scripting.md"
  - "docs/sage_lua.md"
---

# Скрипты и их API

## Стиль API — стандарт SAGE (`docs/scripting.md`, «Стиль API SAGE»)

Для всего нового API, примеров и документации:
- без префикса `sage.` в примерах; имена строчными через `_`;
- методы объекта через точку (`m.vertex(...)`), свойства — присваиванием
  (`stone.roughness = 0.9`);
- настройки одной таблицей (`noise{kind = "simplex"}`), необязательное — в
  конце с умолчанием; без цепочек вызовов.

## Совместимость

Старые имена не удаляются. Снимок `tests/data/script_api_surface.txt`
сторожит поверхность API — менять его только при осознанном добавлении.

## Новая функция — одна регистрация

- Для всех языков: `RegisterApiFunction(модуль, имя, функция, описание)`
  (`sage/scripting/ScriptApi.h`), типы — переносимые. Только для Lua —
  `Bind(...)` в `engine/src/sage/scripting/`.
- Затем `python3 scripts/gen_script_api.py` (нужен собранный
  `build/tools/sage_api`) — обновляет подсказку `editor/assets/api/sage.lua` и
  таблицу проверки типов. Проверка: `python3 scripts/gen_script_api.py --check`.
- Подсказку и таблицу руками не править.

## Новый язык

Запись реестра `RegisterLanguage` (`sage/scripting/Languages.h`) или описание
в `scripting/languages.json` проекта (`sage/scripting/ProcessLanguage.h`), а
не ветки `if (ext == ...)` в инструментах и хостах.

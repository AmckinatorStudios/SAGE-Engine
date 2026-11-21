---
paths:
  - "engine/src/sage/scripting/**"
  - "editor/assets/api/**"
  - "tools/sage_api/**"
  - "tests/data/script_api_surface.txt"
  - "scripts/gen_script_api.py"
  - "**/scripting/languages.json"
  - "docs/scripting.md"
  - "docs/sage_lua.md"
---

# API скриптов

- Стиль — стандарт SAGE (`docs/scripting.md`): без `sage.` в примерах; имена
  строчными через `_`; методы через точку (`m.vertex(...)`); свойства
  присваиванием (`stone.roughness = 0.9`); настройки одной таблицей
  (`noise{kind = "simplex"}`); необязательное — в конце с умолчанием; без
  цепочек вызовов.
- Совместимость: старые имена не удалять; снимок
  `tests/data/script_api_surface.txt` менять только при осознанном добавлении.
- Новая функция — одна регистрация: `RegisterApiFunction(модуль, имя,
  функция, описание)` (`sage/scripting/ScriptApi.h`, типы переносимые); только
  для Lua — `Bind(...)`. Затем `python3 scripts/gen_script_api.py` (нужен
  `build/tools/sage_api`) обновляет `editor/assets/api/sage.lua` и таблицу
  типов — руками их не править; проверка — `--check`.
- Новый язык — `RegisterLanguage` (`sage/scripting/Languages.h`) или
  `scripting/languages.json` проекта (`ProcessLanguage.h`), не
  `if (ext == ...)`.

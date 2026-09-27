# Тесты скриптинга на Lua

Запуск — прямо в редакторе: **Play → Run Lua Tests**. Редактор находит в
проекте все файлы `*.test.lua`, запускает Play, ставит по объекту на файл и
прогоняет методы `test_*` по порядку, каждый — корутиной в настоящих кадрах.
Итог — в консоли (категория `Test`); Play, запущенный ради тестов,
останавливается сам.

Этот набор проверяет сам API скриптинга движка; самопроверка редактора
(`LUA_TESTS`) копирует его в проект и прогоняет на каждом запуске CI;
smoke-тест без вердикта `LUA_TESTS: OK` сборку не пропускает.

    local T = {}
    function T:test_something()
        Test.eq(1 + 1, 2)
        Test.wait(0.5)        -- кадр уходит движку
    end
    return T

Проверки: `Test.expect`, `eq`, `ne`, `near`, `errors`, `fail`.
Ожидание: `Test.wait(сек)`, `Test.frames(n)`, `Test.waitUntil(fn, сек)`, `Test.timeout(сек)`.
Ввод: `Test.click(obj)`, `Test.type(obj, текст)`, `Test.slide(obj, 0..1)`,
`Test.hover(obj | nil)`, `Test.key("W", true/false)`.
Необязательные `before_each` / `after_each` — вокруг каждого теста.

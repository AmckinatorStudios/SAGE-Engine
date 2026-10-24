-- Расстановка — встроенный пример плагина редактора на Lua.
--
-- Окно «Плагины → Расстановка»: выделите объект, выберите узор и нажмите
-- «Расставить». Всё, что плагин создаст, отменяется одним Ctrl+Z.
-- Скопируйте эту папку в «Мои плагины» и правьте как хотите: сохранённый файл
-- подхватывается сам. Справка по API — docs/plugins.md.

local patterns = { "Сетка", "Круг", "Вразброс" }
local pattern = 1
local count = 10        -- сколько копий
local columns = 5       -- для сетки
local spacing = 2.0     -- шаг сетки / радиус круга / размах разброса
local randomYaw = false -- случайный поворот вокруг Y (деревья, камни)

-- Где встанет i-я копия относительно исходного объекта.
local function offset(i)
    if pattern == 1 then
        local col = (i - 1) % columns
        local row = math.floor((i - 1) / columns)
        return col * spacing, 0, row * spacing
    elseif pattern == 2 then
        local a = (i - 1) / count * math.pi * 2
        return math.cos(a) * spacing, 0, math.sin(a) * spacing
    else
        return (math.random() - 0.5) * 2 * spacing, 0, (math.random() - 0.5) * 2 * spacing
    end
end

local function scatter()
    local source = editor.selected()
    if not source then
        editor.status("Расстановка: сначала выделите объект")
        return
    end
    local p = source.position
    local made = {}
    for i = 1, count do
        local copy = editor.duplicate(source)
        local dx, dy, dz = offset(i)
        copy.position = { x = p.x + dx, y = p.y + dy, z = p.z + dz }
        if randomYaw then
            local r = copy.rotation
            copy.rotation = { x = r.x, y = math.random() * 360, z = r.z }
        end
        copy.name = source.name .. " " .. i
        made[#made + 1] = copy
    end
    editor.select(made)
    editor.status("Расстановка: создано копий — " .. #made)
end

editor.panel("Расстановка", function()
    local sel = editor.selected()
    ui.hint(sel and ("Объект: " .. sel.name) or "Выделите объект в сцене")
    pattern = ui.combo("Узор", pattern, patterns)
    count = ui.int("Копий", count, 1, 200)
    if pattern == 1 then columns = ui.int("В ряду", columns, 1, 50) end
    spacing = ui.slider(pattern == 1 and "Шаг" or (pattern == 2 and "Радиус" or "Размах"), spacing, 0.1, 50)
    randomYaw = ui.checkbox("Случайный поворот", randomYaw)
    ui.space()
    if ui.button("Расставить") then scatter() end
    ui.tooltip("Копии появятся рядом с выделенным объектом и станут выделенными")
end)

editor.menu("Расставить копии (последние настройки)", scatter)

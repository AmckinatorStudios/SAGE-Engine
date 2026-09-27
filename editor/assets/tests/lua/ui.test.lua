-- Интерфейс как обычный объект: щелчок настоящим путём (Test.click проходит
-- через тот же UpdateSceneUI, что мышь), свойства text/visible/enabled.
local U = {}

function U:after_each()
    for _, name in ipairs({"LT_Play", "LT_Box", "LT_Slider", "LT_Field", "LT_Panel"}) do
        local o = UI.find(name)
        if o then o:Destroy() end
    end
end

function U:test_button_clicked_reaches_the_script()
    local b = UI.create("Button", "LT_Play")
    local n, sender = 0, nil
    b.clicked:connect(function(event) n = n + 1; sender = event.sender end)
    Test.expect(Test.click(b), "кнопки нет на экране")
    Test.eq(n, 1)
    Test.eq(sender.id, b.id)
end

function U:test_press_release_and_hover_signals()
    local b = UI.create("Button", "LT_Play")
    local log = {}
    for _, s in ipairs({"pressed", "released", "clicked", "hovered", "unhovered"}) do
        b[s]:connect(function() log[#log + 1] = s end)
    end
    Test.hover(b)
    Test.click(b)
    Test.hover(nil)
    local text = table.concat(log, ",")
    Test.expect(text:find("hovered", 1, true), "нет hovered: " .. text)
    Test.expect(text:find("pressed,released,clicked", 1, true), "порядок нажатия: " .. text)
    Test.expect(text:find("unhovered", 1, true), "нет unhovered: " .. text)
end

function U:test_hidden_or_disabled_button_does_not_click()
    local b = UI.create("Button", "LT_Play")
    local n = 0
    b.clicked:connect(function() n = n + 1 end)
    b:hide()
    Test.eq(b.visible, false)
    Test.eq(Test.click(b), false, "спрятанная кнопка нашлась на экране")
    b:show()
    b:set_enabled(false)
    Test.eq(b.enabled, false)
    Test.click(b)
    Test.eq(n, 0, "выключенная кнопка нажалась")
    b.enabled = true
    Test.click(b)
    Test.eq(n, 1)
end

function U:test_text_property_changes_the_label()
    local b = UI.create("Button", "LT_Play")
    Test.expect(b.text ~= nil, "у кнопки нет надписи")
    b:set_text("Играть")
    Test.eq(b.text, "Играть")
    b.text = "Ещё раз"
    Test.eq(b.text, "Ещё раз")
end

function U:test_checkbox_reports_value_changed()
    local box = UI.create("Checkbox", "LT_Box")
    local values = {}
    box.value_changed:connect(function(event, v) values[#values + 1] = v end)
    local before = box.value
    Test.click(box)
    Test.eq(#values, 1, "value_changed не пришёл")
    Test.eq(values[1], not before)
    Test.eq(box.value, not before)
end

function U:test_slider_follows_the_mouse()
    local s = UI.create("Slider", "LT_Slider")
    local last
    s.value_changed:connect(function(event, v) last = v end)
    Test.expect(Test.slide(s, 0.9), "ползунка нет на экране")
    Test.expect(last ~= nil, "value_changed не пришёл")
    Test.expect(s.value > 0.7, "значение ползунка " .. tostring(s.value))
    s.value = 0.25
    Test.near(last, 0.25, 1e-4, "запись value не прислала value_changed")
end

function U:test_text_field_reports_typing()
    local f = UI.create("Input Field", "LT_Field")
    local got
    f.text_changed:connect(function(event, t) got = t end)
    Test.expect(Test.type(f, "abc"), "поля нет на экране")
    Test.eq(got, "abc")
    Test.eq(f.text, "abc")
end

function U:test_get_and_find()
    local p = UI.create("Panel", "LT_Panel")
    Test.eq(UI.get("LT_Panel").id, p.id)
    Test.eq(UI.find("LT_Nope"), nil)
    Test.errors(function() UI.get("LT_Nope") end, "не найден")
    Test.errors(function() UI.create("Teapot") end, "Teapot")
end

return U

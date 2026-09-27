-- Ввод: клавиша, положенная в очередь окна (Test.key), видна игре со
-- следующего кадра — как настоящая.
local I = {}

function I:after_each()
    Test.key("W", false)
    Test.key("Space", false)
    Test.frames(1)
end

function I:test_key_down_and_up()
    Test.key("W", true)
    Test.frames(1)
    Test.expect(Input.IsKeyDown("W"), "W не нажата через кадр")
    Test.key("W", false)
    Test.frames(1)
    Test.expect(not Input.IsKeyDown("W"), "W не отпущена")
end

function I:test_action_bound_by_the_script()
    Test.expect(Input.BindAction("LT_Jump", "Space"), "BindAction не принял клавишу")
    Test.key("Space", true)
    Test.frames(1)
    Test.expect(Input.IsActionDown("LT_Jump"), "действие не нажато")
    Test.key("Space", false)
    Test.frames(1)
    Test.expect(not Input.IsActionDown("LT_Jump"), "действие не отпущено")
end

function I:test_unknown_key_is_an_error()
    Test.eq(Input.IsKeyDown("NoSuchKey"), false)
    Test.errors(function() Test.key("NoSuchKey") end, "NoSuchKey")
end

return I

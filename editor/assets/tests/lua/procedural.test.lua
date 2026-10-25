-- Процедурная генерация в настоящем Play — в стиле SAGE: простые имена,
-- объекты через точку, свойства присваиванием. Модульные тесты проверяют то
-- же без окна; здесь — что меш правда встаёт на объект, текстура грузится на
-- видеокарту, а кадр с ними рисуется.
local P = {}

function P:test_noise_and_formula()
    local n = noise{kind = "perlin", seed = 3, octaves = 4}
    Test.eq(n(1.5, 2.5), noise{kind = "perlin", seed = 3, octaves = 4}(1.5, 2.5))
    local f = formula("lerp(a, b, t) + clamp(t, 0, 1)", "a, b, t")
    Test.near(f(0, 10, 0.5), 5.5, 1e-4)
    Test.errors(function() formula("1 +", "x") end, "Formula")
end

function P:test_mesh_goes_onto_object()
    local o = Scene.Create("LT_Terrain")
    local n = noise{seed = 7, scale = 0.2}
    local ground = grid(8, 8, 16, 16, function(x, z) return n(x, z) * 2 end)
    Test.eq(ground.triangles, 16 * 16 * 2)
    ground.apply(o)
    Test.expect(o:HasComponent("Mesh"), "меш не встал на объект")
    Test.frames(2)                      -- объект с новым мешем рисуется без ошибок
    Test.errors(function() mesh().apply(o) end, "треугольник")
    o:Destroy()
end

function P:test_material_and_texture_from_code()
    local tex = texture("lt_proc_tex", 8, 8)
    tex.fill(function(u, v) return rgb(u, v, 1) end)
    local stone = material("lt_proc_mat", {color = rgb(1, 0, 0), roughness = 0.3})
    stone.texture = tex                 -- текстура грузится на видеокарту здесь
    Test.eq(stone.roughness, 0.3)
    Test.errors(function() stone.no_such_key = 1 end, "no_such_key")
    -- Прежний стиль — те же функции: старые скрипты работают как работали.
    Test.eq(sage.noise.Perlin(0.25, 0.75), perlin(0.25, 0.75))
end

return P

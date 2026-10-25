-- Процедурная генерация в настоящем Play: шум, формула, меш на объекте,
-- материал и текстура из кода. Модульные тесты проверяют это без окна;
-- здесь — что меш правда встаёт на объект и рисуется в кадре.
local P = {}

function P:test_noise_and_formula()
    local n = sage.noise.New{ type = "perlin", seed = 3, octaves = 4 }
    Test.eq(n:Fbm(1.5, 2.5), sage.noise.New{ type = "perlin", seed = 3, octaves = 4 }:Fbm(1.5, 2.5))
    local f = sage.math.Formula("lerp(a, b, t) + clamp(t, 0, 1)", "a, b, t")
    Test.near(f(0, 10, 0.5), 5.5, 1e-4)
    Test.errors(function() sage.math.Formula("1 +", "x") end, "Formula")
end

function P:test_mesh_goes_onto_object()
    local o = Scene.Create("LT_Terrain")
    local n = sage.noise.New{ seed = 7, frequency = 0.2 }
    local g = sage.mesh.Grid(8, 8, 16, 16, function(x, z) return n:Fbm(x, 0, z) * 2 end)
    Test.eq(g:TriangleCount(), 16 * 16 * 2)
    g:Apply(o)
    Test.expect(o:HasComponent("Mesh"), "меш не встал на объект")
    Test.frames(2)                      -- объект с новым мешем рисуется без ошибок
    Test.errors(function() sage.mesh.New():Apply(o) end, "треугольник")
    o:Destroy()
end

function P:test_material_and_texture_from_code()
    sage.material.Create("lt_proc_mat", { albedo = Vec3(1, 0, 0), roughness = 0.3 })
    local t = sage.texture.New("lt_proc_tex", 8, 8)
    t:Fill(function(u, v) return Vec3(u, v, 1) end)
    Test.eq(t:Apply(), "lt_proc_tex")
    sage.material.Set("lt_proc_mat", "texture", "lt_proc_tex")
    Test.errors(function() sage.material.Set("lt_proc_mat", "no_such_key", 1) end, "no_such_key")
end

return P

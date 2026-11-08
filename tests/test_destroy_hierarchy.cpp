#include "TestFramework.h"

#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"

// Удаление ветки иерархии из скрипта в том порядке, в каком его делает игра
// (модель сундука: защёлка — дочерняя крышки, крышка и корпус — корня):
// сначала внук, потом его родитель, потом корень; тут же создаётся новая
// такая же модель. Повторяется много раз — со стороны это было падением
// игры на Windows при постановке второго сундука рядом с первым.
TEST(Destroy_hierarchy_child_first_then_parents_from_script) {
    Scene scene("S");
    ScriptEngine se;
    se.BindScene(scene);
    auto r = se.Lua().safe_script(R"LUA(
        local function create()
            local m = {}
            m.root = SpawnObject("Chest")
            m.base = SpawnObject("Base"); m.base:SetParent(m.root)
            m.lid = SpawnObject("Lid"); m.lid:SetParent(m.root)
            m.knob = SpawnObject("Lock"); m.knob:SetParent(m.lid)
            return m
        end
        local function destroy(m)
            DestroyObject(m.knob); DestroyObject(m.lid); DestroyObject(m.base); DestroyObject(m.root)
        end
        local models = {}
        for i = 1, 40 do models[i] = create() end
        for round = 1, 200 do
            local i = (round * 7) % 40 + 1
            destroy(models[i])
            models[i] = create()
            -- И наоборот: корень первым (потомки уходят с ним), затем — уже
            -- удалённые потомки (их удаление — не ошибка).
            local j = (round * 13) % 40 + 1
            DestroyObject(models[j].root)
            DestroyObject(models[j].knob); DestroyObject(models[j].lid)
            models[j] = create()
        end
        local n = 0
        for _ in pairs(models) do n = n + 1 end
        return n
    )LUA", sol::script_pass_on_error);
    CHECK_TRUE(r.valid());
    // Сцена цела: у каждого живого объекта родитель (если есть) жив и знает его.
    int broken = 0;
    auto& reg = scene.Registry();
    for (auto e : reg.view<HierarchyComponent>()) {
        const auto& h = reg.get<HierarchyComponent>(e);
        if (h.Parent != entt::null) {
            if (!reg.valid(h.Parent)) { ++broken; continue; }
            const auto* ph = reg.try_get<HierarchyComponent>(h.Parent);
            if (!ph || std::find(ph->Children.begin(), ph->Children.end(), e) == ph->Children.end()) ++broken;
        }
        for (auto c : h.Children) if (!reg.valid(c)) ++broken;
    }
    CHECK_EQ(broken, 0);
}

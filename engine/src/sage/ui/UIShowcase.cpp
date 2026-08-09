#include "sage/ui/UIShowcase.h"

#include <string>
#include <vector>

#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"
#include "sage/ui/UIBridge.h"

namespace sage::ui {

namespace {

using UI = UIElementComponent;

// Создаёт UI-сущность и (опционально) родителя; возвращает её.
//
// Описание элемента здесь остаётся ПЛОСКИМ, а в сцену кладётся разбором на
// компоненты. Это витрина: она нарочно набита элементами всех видов, и
// расписывать каждый из полусотни набором компонентов значило бы утроить файл,
// ничего не показав сверх того, что он показывает сейчас. Как выглядит сборка
// ИЗ компонентов, показывают демо-сцены (assets/demos) и заготовки UI.h.
GameObject MakeUI(Scene& scene, const std::string& name, const UI& ui, GameObject parent) {
    GameObject e = scene.CreateObject(name);
    Decompose(ui, scene.Registry(), e.Entity());
    if (parent.Valid()) scene.SetParent(e.Entity(), parent.Entity());
    return e;
}

UI Panel(UIAnchor a, glm::vec2 off, glm::vec2 size, glm::vec4 color, float rounding = 8.0f) {
    UI u;
    u.Type = UI::Kind::Panel;
    u.Anchor = a; u.Offset = off; u.Size = size; u.Color = color; u.Rounding = rounding;
    return u;
}

UI TextLine(UIAnchor a, glm::vec2 off, const std::string& text, float scale, glm::vec4 col) {
    UI u;
    u.Type = UI::Kind::Label;
    u.Anchor = a; u.Offset = off; u.Size = {180.0f, 24.0f};
    u.Text = text; u.TextScale = scale; u.TextColor = col; u.TextCentered = false;
    return u;
}

} // namespace

int BuildShowcase(Scene& scene) {
    // Корень — ПРОСТАЯ сущность без UIElementComponent: только группировка (и
    // удаление всего интерфейса разом). Дети с UI считаются экранными корнями
    // (родитель без UI => якорь к экрану), сам корень не рисуется и не кликается.
    GameObject root = scene.CreateObject("UIShowcase");

    // ======================= ИНВЕНТАРЬ (справа) =======================
    UI invUi = Panel(UIAnchor::TopRight, {24, 24}, {372, 470}, {0.08f, 0.09f, 0.13f, 0.92f}, 12.0f);
    invUi.BorderThickness = 2.0f; invUi.BorderColor = {0.85f, 0.80f, 0.62f, 0.9f};
    GameObject inv = MakeUI(scene, "Inventory", invUi, root);

    MakeUI(scene, "InvTitle", TextLine(UIAnchor::TopLeft, {18, 14}, "INVENTORY", 3.0f, {1, 0.95f, 0.8f, 1}), inv);

    // Обрезающий контейнер-«вьюпорт» сетки (ClipChildren — маска): слоты за его
    // границей не рисуются (список прокручиваемых предметов).
    // Высота меньше, чем полная сетка (5 рядов) — нижние ряды реально обрезаются
    // (прокручиваемый список): маска работает и в рендере, и в hit-тесте.
    UI gridUi = Panel(UIAnchor::TopLeft, {16, 58}, {340, 250}, {0.05f, 0.06f, 0.09f, 0.9f}, 8.0f);
    gridUi.ClipChildren = true;
    GameObject grid = MakeUI(scene, "InvGrid", gridUi, inv);

    // 5×5 слотов (25 — часть заведомо за нижней границей маски: проверка обрезки).
    const glm::vec4 kItemColors[] = {
        {0.80f, 0.35f, 0.30f, 1}, {0.35f, 0.65f, 0.85f, 1}, {0.85f, 0.78f, 0.35f, 1},
        {0.55f, 0.80f, 0.45f, 1}, {0.75f, 0.45f, 0.80f, 1},
    };
    int slotIndex = 0;
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 5; ++col, ++slotIndex) {
            float x = 12.0f + col * 64.0f;
            float y = 12.0f + row * 64.0f;
            UI slotUi = Panel(UIAnchor::TopLeft, {x, y}, {56, 56}, {0.14f, 0.15f, 0.20f, 1.0f}, 6.0f);
            // Один слот выделен (толще/ярче рамка) — состояние выбора.
            if (slotIndex == 7) { slotUi.BorderThickness = 3.0f; slotUi.BorderColor = {1, 0.85f, 0.3f, 1}; }
            GameObject slot = MakeUI(scene, "Slot" + std::to_string(slotIndex), slotUi, grid);

            // Иконка предмета (не у всех слотов — часть пустая).
            if ((slotIndex * 7 + 3) % 5 != 0) {
                UI icon = Panel(UIAnchor::Center, {0, 0}, {40, 40},
                                kItemColors[slotIndex % 5], 6.0f);
                icon.Type = UI::Kind::Panel;
                MakeUI(scene, "Icon" + std::to_string(slotIndex), icon, slot);
                // Счётчик в углу.
                UI cnt = TextLine(UIAnchor::BottomRight, {4, 2},
                               "x" + std::to_string((slotIndex * 3) % 9 + 1), 1.4f, {1, 1, 1, 1});
                cnt.Size = {28, 16};
                MakeUI(scene, "Count" + std::to_string(slotIndex), cnt, slot);
            }
        }
    }

    // Полоса веса внизу инвентаря.
    MakeUI(scene, "WeightLabel", TextLine(UIAnchor::BottomLeft, {18, 40}, "Weight", 1.6f, {0.8f, 0.85f, 0.9f, 1}), inv);
    UI weightBar = Panel(UIAnchor::BottomLeft, {18, 14}, {336, 20}, {0.10f, 0.11f, 0.15f, 1.0f}, 6.0f);
    weightBar.Type = UI::Kind::Bar;
    weightBar.Value = 0.62f;
    weightBar.BarFillColor = {0.90f, 0.60f, 0.25f, 1.0f};
    MakeUI(scene, "WeightBar", weightBar, inv);

    // ======================= ДЕРЕВО НАВЫКОВ (слева) =======================
    UI skillUi = Panel(UIAnchor::TopLeft, {24, 24}, {430, 470}, {0.08f, 0.10f, 0.12f, 0.92f}, 12.0f);
    skillUi.BorderThickness = 2.0f; skillUi.BorderColor = {0.55f, 0.80f, 0.95f, 0.9f};
    GameObject skills = MakeUI(scene, "SkillTree", skillUi, root);

    MakeUI(scene, "SkillTitle", TextLine(UIAnchor::TopLeft, {18, 14}, "SKILL TREE  -  3 pts", 2.6f, {0.75f, 0.9f, 1, 1}), skills);

    // Узлы дерева (позиции внутри панели) + связи родитель->ребёнок.
    struct Node { const char* Name; float X, Y; int Parent; bool Unlocked; };
    const Node nodes[] = {
        {"Root",   195, 70,  -1, true},
        {"Might",  95,  170, 0,  true},
        {"Guard",  295, 170, 0,  false},
        {"Cleave", 45,  280, 1,  true},
        {"Rage",   150, 280, 1,  false},
        {"Bulwark",250, 280, 2,  false},
        {"Aegis",  345, 280, 2,  false},
        {"Ultima", 195, 390, 3,  false},
    };
    constexpr int kNodeCount = (int)(sizeof(nodes) / sizeof(nodes[0]));
    const float kNodeSize = 58.0f, kHalf = kNodeSize * 0.5f;

    // Провода (тонкие панели-сегменты) — рисуем ПОД узлами (меньший Layer), от
    // центра родителя к центру ребёнка Г-образно (вертикаль + горизонталь).
    for (int i = 0; i < kNodeCount; ++i) {
        if (nodes[i].Parent < 0) continue;
        const Node& p = nodes[nodes[i].Parent];
        const Node& c = nodes[i];
        float pcx = p.X + kHalf, pcy = p.Y + kHalf;
        float ccx = c.X + kHalf, ccy = c.Y + kHalf;
        glm::vec4 wire = c.Unlocked ? glm::vec4(0.55f, 0.85f, 1.0f, 0.9f)
                                    : glm::vec4(0.30f, 0.34f, 0.40f, 0.9f);
        // Вертикальный сегмент от родителя вниз до уровня ребёнка.
        float vTop = pcy, vBot = ccy, vh = vBot - vTop;
        UI vSeg = Panel(UIAnchor::TopLeft, {pcx - 2.0f, vTop}, {4.0f, vh > 0 ? vh : 1.0f}, wire, 2.0f);
        vSeg.Layer = -1;
        MakeUI(scene, std::string("WireV") + std::to_string(i), vSeg, skills);
        // Горизонтальный сегмент на уровне ребёнка от родителя-X к ребёнку-X.
        float hL = pcx < ccx ? pcx : ccx, hR = pcx < ccx ? ccx : pcx, hw = hR - hL;
        UI hSeg = Panel(UIAnchor::TopLeft, {hL - 2.0f, ccy - 2.0f}, {hw + 4.0f, 4.0f}, wire, 2.0f);
        hSeg.Layer = -1;
        MakeUI(scene, std::string("WireH") + std::to_string(i), hSeg, skills);
    }

    // Сами узлы (поверх проводов) — круглые (Rounding=половина), с надписью.
    for (int i = 0; i < kNodeCount; ++i) {
        const Node& n = nodes[i];
        glm::vec4 fill = n.Unlocked ? glm::vec4(0.20f, 0.45f, 0.70f, 1.0f)
                                    : glm::vec4(0.14f, 0.16f, 0.20f, 1.0f);
        UI node = Panel(UIAnchor::TopLeft, {n.X, n.Y}, {kNodeSize, kNodeSize}, fill, kHalf);
        node.Layer = 1;
        node.BorderThickness = 2.0f;
        node.BorderColor = n.Unlocked ? glm::vec4(0.6f, 0.9f, 1.0f, 1.0f)
                                      : glm::vec4(0.35f, 0.38f, 0.44f, 1.0f);
        node.Text = n.Name;
        node.TextScale = 1.3f;
        node.TextCentered = true;
        node.TextColor = n.Unlocked ? glm::vec4(1, 1, 1, 1) : glm::vec4(0.6f, 0.62f, 0.68f, 1.0f);
        MakeUI(scene, std::string("Skill_") + n.Name, node, skills);
    }

    return root.Id();
}

} // namespace sage::ui

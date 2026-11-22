// ===========================================================================
//  EDITOR UI KIT (editor/src/InspectorControls*.cpp) — настоящими кадрами
//  ImGui без окна.
//
//  Главное правило kit: одна функция — один цельный контрол. Проверяем то,
//  что видно снаружи: вектор — одна строка ровно во всю ширину, сегменты без
//  чужих идентификаторов; список открывается щелчком по любому месту поля, в
//  том числе там, где раньше была отдельная кнопка-стрелка; группа кнопок
//  отдаёт номер нажатого сегмента; узкая панель переводит вектор в Compact,
//  а потом в Stacked, а не сжимает числа до нечитаемых.
// ===========================================================================
#include "TestFramework.h"

#include <cstdio>
#include <functional>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"

#include "../editor/src/EditorTheme.h"
#include "../editor/src/InspectorControls.h"

using namespace sage::editor;

namespace {

// Кадр ImGui с окном w×h в левом верхнем углу; draw — содержимое окна.
struct KitFrame {
    ImGuiContext* Ctx = nullptr;
    KitFrame() {
        Ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(800, 600);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    }
    ~KitFrame() { ImGui::DestroyContext(Ctx); }
    void Run(float width, const std::function<void()>& draw) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, 500));
        ImGui::Begin("Kit", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration);
        draw();
        ImGui::End();
        ImGui::Render();
    }
    void Click(float x, float y, float width, const std::function<void()>& draw) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);
        Run(width, draw);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        Run(width, draw);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        Run(width, draw);
    }
};

} // namespace

TEST(UiKit_vector_look_follows_the_width) {
    const float h = 24.0f, tag = 18.0f;
    // Широко — сегменты с −/+; уже — без кнопок; совсем узко — ось на строку.
    CHECK_TRUE(ictl::FitVecLook(ictl::VecLook::Segmented, 600.0f, 3, tag, h) == ictl::VecLook::Segmented);
    CHECK_TRUE(ictl::FitVecLook(ictl::VecLook::Segmented, 240.0f, 3, tag, h) == ictl::VecLook::Compact);
    // Число не обрезается: строка, в которую не влезает «-000.000» на ось
    // даже без −/+, уходит в столбик, а не показывает «-1.60(».
    CHECK_TRUE(ictl::FitVecLook(ictl::VecLook::Segmented, 200.0f, 3, tag, h) == ictl::VecLook::Stacked);
    CHECK_TRUE(ictl::FitVecLook(ictl::VecLook::Segmented, 90.0f, 3, tag, h) == ictl::VecLook::Stacked);
    // Заказанный вид не «повышается»: Compact в широкой панели остаётся Compact,
    // Stacked — Stacked.
    CHECK_TRUE(ictl::FitVecLook(ictl::VecLook::Compact, 600.0f, 3, tag, h) == ictl::VecLook::Compact);
    CHECK_TRUE(ictl::FitVecLook(ictl::VecLook::Stacked, 600.0f, 3, tag, h) == ictl::VecLook::Stacked);
    // Четыре оси требуют больше места, чем две.
    CHECK_TRUE(ictl::FitVecLook(ictl::VecLook::Segmented, 300.0f, 2, tag, h) == ictl::VecLook::Segmented);
    CHECK_TRUE(ictl::FitVecLook(ictl::VecLook::Segmented, 300.0f, 4, tag, h) != ictl::VecLook::Segmented);
}

TEST(UiKit_vector_is_one_row_without_id_clashes) {
    KitFrame f;
    float v[3] = {1.0f, 2.0f, 3.0f};
    ImVec2 rect(0, 0);
    int worst = 0;
    auto draw = [&] {
        ictl::DragVec(nullptr, "##vec", v, 3, 0.1f);
        rect = ImGui::GetItemRectSize();
    };
    for (float x = 4.0f; x < 590.0f; x += 7.0f) {
        ImGui::GetIO().AddMousePosEvent(x, 18.0f);
        f.Run(600.0f, draw);
        f.Run(600.0f, draw);
        worst = std::max(worst, f.Ctx->HoveredIdPreviousFrameItemCount);
    }
    // Строка полей — во всю ширину (значок справа встаёт за последним полем),
    // высотой в одно поле.
    const float avail = 600.0f - 2.0f * ImGui::GetStyle().WindowPadding.x;
    CHECK_NEAR(rect.x, avail, 1.0f);
    CHECK_NEAR(rect.y, ImGui::GetFrameHeight(), 0.5f);
    CHECK_TRUE(worst <= 1);
}

// ОСЬ — СВОЁ ПОЛЕ: у каждой оси своя область взаимодействия, поля одной
// ширины не пересекаются и разделены отступом, в отступе ничего не нажимается.
// Для векторов 2, 3 и 4 осей.
TEST(UiKit_vector_axes_are_separate_fields) {
    for (const int n : {2, 3, 4}) {
        KitFrame f;
        float v[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        auto draw = [&] { ictl::DragVec(nullptr, "##vec", v, n, 0.1f); };
        f.Run(700.0f, draw);
        const ImVec2 pad = ImGui::GetStyle().WindowPadding;
        const float avail = 700.0f - 2.0f * pad.x;
        const float field = ictl::VecFieldWidth(avail, n);
        const float gap = ictl::VecAxisGap();
        CHECK_TRUE(gap >= 2.0f);
        CHECK_TRUE(field * (float)n + gap * (float)(n - 1) <= avail + 0.5f);
        const float y = pad.y + ImGui::GetFrameHeight() * 0.5f;
        auto hoveredAt = [&](float x) {
            ImGui::GetIO().AddMousePosEvent(x, y);
            f.Run(700.0f, draw);
            f.Run(700.0f, draw);
            return f.Ctx->HoveredId;
        };
        ImGuiID ids[4] = {};
        for (int i = 0; i < n; ++i) {
            const float x0 = pad.x + (field + gap) * (float)i;
            // Середина числа в поле — само поле оси.
            ids[i] = hoveredAt(x0 + field * 0.6f);
            CHECK_TRUE(ids[i] != 0);
            for (int k = 0; k < i; ++k) CHECK_TRUE(ids[i] != ids[k]);
            // Отступ после поля — пустой: поля не соприкасаются.
            if (i + 1 < n) CHECK_EQ(hoveredAt(x0 + field + gap * 0.5f), (ImGuiID)0);
        }
    }
}

// Каждую ось правят независимо: перетаскивание по полю Y меняет только Y.
TEST(UiKit_vector_axes_change_independently) {
    KitFrame f;
    float v[3] = {1.0f, 2.0f, 3.0f};
    auto draw = [&] { ictl::DragVec(nullptr, "##vec", v, 3, 0.1f); };
    f.Run(700.0f, draw);
    const ImVec2 pad = ImGui::GetStyle().WindowPadding;
    const float field = ictl::VecFieldWidth(700.0f - 2.0f * pad.x, 3);
    const float x = pad.x + field + ictl::VecAxisGap() + field * 0.6f;
    const float y = pad.y + ImGui::GetFrameHeight() * 0.5f;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    f.Run(700.0f, draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    f.Run(700.0f, draw);
    for (int step = 1; step <= 6; ++step) {
        io.AddMousePosEvent(x + 10.0f * (float)step, y);
        f.Run(700.0f, draw);
    }
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    f.Run(700.0f, draw);
    CHECK_TRUE(v[1] > 2.0f);
    CHECK_NEAR(v[0], 1.0f, 1e-6f);
    CHECK_NEAR(v[2], 3.0f, 1e-6f);
}

// Узкая панель — ось на строку: поля не сжимаются до нечитаемых.
TEST(UiKit_vector_stacks_when_narrow) {
    KitFrame f;
    float v[3] = {1.0f, 2.0f, 3.0f};
    ImVec2 rect(0, 0);
    auto draw = [&] {
        ictl::DragVec(nullptr, "##vec", v, 3, 0.1f);
        rect = ImGui::GetItemRectSize();
    };
    f.Run(110.0f, draw);
    CHECK_TRUE(rect.y > ImGui::GetFrameHeight() * 2.5f);
    f.Run(700.0f, draw);
    CHECK_NEAR(rect.y, ImGui::GetFrameHeight(), 0.5f);
}

TEST(UiKit_dropdown_opens_from_the_whole_field) {
    // Щелчок у правого края — там, где у ImGui была отдельная кнопка со
    // стрелкой, — и посередине: оба открывают список.
    for (const float x : {20.0f, 150.0f, 290.0f}) {
        KitFrame f;
        int current = 0;
        const char* items[3] = {"One", "Two", "Three"};
        bool opened = false;
        auto draw = [&] {
            ImGui::SetNextItemWidth(300.0f);
            if (ictl::BeginDropdown("##dd", items[current])) {
                opened = true;
                ictl::EndDropdown();
            }
        };
        f.Run(400.0f, draw);
        const float y = ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight() * 0.5f;
        f.Click(ImGui::GetStyle().WindowPadding.x + x, y, 400.0f, draw);
        f.Run(400.0f, draw);
        CHECK_TRUE(opened);
        // Окно списка на экране: есть, видно, ненулевого размера.
        const ImGuiWindow* popup = nullptr;
        for (ImGuiWindow* w : f.Ctx->Windows)
            if ((w->Flags & ImGuiWindowFlags_Popup) && w->Active) popup = w;
        CHECK_TRUE(popup != nullptr);
        if (popup) {
            CHECK_TRUE(popup->Size.x > 10.0f && popup->Size.y > 10.0f);
        }
    }
}

TEST(UiKit_button_group_reports_the_pressed_segment) {
    KitFrame f;
    ictl::GroupItem items[3];
    items[0].Text = "Left";
    items[1].Text = "Middle";
    items[2].Text = "Right";
    int last = -1;
    auto draw = [&] {
        const int hit = ictl::ButtonGroup("##grp", items, 3, ictl::ButtonLook::Standard, 0, 300.0f);
        if (hit >= 0) last = hit;
    };
    f.Run(400.0f, draw);
    const ImVec2 pad = ImGui::GetStyle().WindowPadding;
    const float y = pad.y + ImGui::GetFrameHeight() * 0.5f;
    // Равные сегменты по 100: центр каждого — его номер.
    for (int i = 0; i < 3; ++i) {
        f.Click(pad.x + 50.0f + 100.0f * (float)i, y, 400.0f, draw);
        CHECK_EQ(last, i);
    }
    // Выключенный сегмент не нажимается.
    items[1].Enabled = false;
    last = -1;
    f.Click(pad.x + 150.0f, y, 400.0f, draw);
    CHECK_EQ(last, -1);
}

TEST(UiKit_grid_group_maps_rows_and_columns) {
    KitFrame f;
    ictl::GroupItem items[9];
    int last = -1;
    auto draw = [&] {
        const int hit = ictl::ButtonGroup("##grid", items, 9, ictl::ButtonLook::IconOnly, 3);
        if (hit >= 0) last = hit;
    };
    f.Run(400.0f, draw);
    const ImVec2 pad = ImGui::GetStyle().WindowPadding;
    const float cell = ImGui::GetFrameHeight();
    // Нижняя правая ячейка сетки 3×3 — восьмая.
    f.Click(pad.x + cell * 2.5f, pad.y + cell * 2.5f, 400.0f, draw);
    CHECK_EQ(last, 8);
    f.Click(pad.x + cell * 1.5f, pad.y + cell * 0.5f, 400.0f, draw);
    CHECK_EQ(last, 1);
}

TEST(UiKit_metrics_follow_the_ui_scale) {
    // Один параметр kit — и все контролы растут вместе с интерфейсом.
    const EditorTheme::Kit one = EditorTheme::ScaledKit(1.0f, 6.0f, 1.0f, 16.0f);
    const EditorTheme::Kit two = EditorTheme::ScaledKit(2.0f, 6.0f, 1.0f, 16.0f);
    CHECK_NEAR(two.LabelMin, one.LabelMin * 2.0f, 1.0f);
    CHECK_NEAR(two.FieldMin, one.FieldMin * 2.0f, 1.0f);
    CHECK_NEAR(two.IconSize, 32.0f, 0.5f);
    CHECK_NEAR(two.SeparatorThickness, 2.0f, 0.5f);
    // Доли высоты поля от масштаба не зависят: поле растёт вместе с кеглем.
    CHECK_NEAR(two.StepButton, one.StepButton, 1e-6f);
    CHECK_NEAR(two.Rounding, 6.0f, 1e-6f);
}

TEST(UiKit_dropdown_with_input_stays_open) {
    KitFrame f;
    bool opened = false;
    static char buf[64] = "";
    auto draw = [&] {
        ImGui::SetNextItemWidth(200.0f);
        opened = false;
        if (ictl::BeginDropdown("##tag", "none")) {
            opened = true;
            ImGui::Selectable("none", true);
            ImGui::Separator();
            ImGui::SetNextItemWidth(150.0f);
            ImGui::InputTextWithHint("##new_tag", "New tag", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue);
            ictl::EndDropdown();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("tip");
    };
    f.Run(400.0f, draw);
    const float y = ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight() * 0.5f;
    f.Click(ImGui::GetStyle().WindowPadding.x + 50.0f, y, 400.0f, draw);
    for (int i = 0; i < 5; ++i) f.Run(400.0f, draw);
    CHECK_TRUE(opened);
    const ImGuiWindow* popup = nullptr;
    for (ImGuiWindow* w : f.Ctx->Windows)
        if ((w->Flags & ImGuiWindowFlags_Popup) && w->Active) popup = w;
    CHECK_TRUE(popup != nullptr);
    CHECK_TRUE(popup && popup->Size.y > 30.0f);
}

// СТРОКА ПОДГРУППЫ С ДЕЙСТВИЯМИ: карандаш и корзина — свои области нажатия,
// щелчок по ним не сворачивает группу; переименование — полем на месте
// подписи, Enter подтверждает, Escape отменяет.
TEST(UiKit_subsection_actions_do_not_toggle_and_rename_in_place) {
    KitFrame f;
    bool rename = false, remove = false, open = false;
    bool editing = false;
    std::string name = "Combat";
    int result = 0;
    auto draw = [&] {
        ictl::SubSectionActions a;
        a.Rename = &rename;
        a.Remove = &remove;
        if (editing) {
            a.EditName = &name;
            a.EditResult = &result;
        }
        open = ictl::SubSection("uikit.test.subsection.actions", "Combat", true, {}, &a);
        if (open) ictl::EndSubSection();
    };
    f.Run(400.0f, draw);
    CHECK_TRUE(open);
    const ImVec2 pad = ImGui::GetStyle().WindowPadding;
    const float h = ImGui::GetFrameHeight();
    const float w = 400.0f - 2.0f * pad.x;
    const float buttons = 2.0f * h + 2.0f + 2.0f;
    const float y = pad.y + h * 0.5f;
    const float pencilX = pad.x + w - buttons + 2.0f + h * 0.5f;
    const float trashX = pencilX + h + 2.0f;

    bool sawRename = false, sawRemove = false;
    auto drawSeen = [&] { draw(); sawRename = sawRename || rename; sawRemove = sawRemove || remove; };
    f.Click(pencilX, y, 400.0f, drawSeen);
    CHECK_TRUE(sawRename);
    CHECK_TRUE(!sawRemove);
    CHECK_TRUE(open);   // группа не свернулась
    f.Click(trashX, y, 400.0f, drawSeen);
    CHECK_TRUE(sawRemove);
    CHECK_TRUE(open);

    // Переименование: поле получает фокус само, набранное приходит в name.
    editing = true;
    f.Run(400.0f, draw);
    f.Run(400.0f, draw);
    ImGuiIO& io = ImGui::GetIO();
    io.AddInputCharactersUTF8("Boss");
    f.Run(400.0f, draw);
    CHECK_EQ(result, 0);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    int got = 0;
    auto drawResult = [&] { draw(); if (result != 0) got = result; };
    f.Run(400.0f, drawResult);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    f.Run(400.0f, drawResult);
    CHECK_EQ(got, 1);
    CHECK_EQ(name, std::string("Boss"));   // всё выделено при входе — набор заменил имя

    // Escape — отмена.
    name = "Combat";
    got = 0;
    f.Run(400.0f, drawResult);
    f.Run(400.0f, drawResult);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    f.Run(400.0f, drawResult);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    f.Run(400.0f, drawResult);
    CHECK_EQ(got, -1);
}

// Полоса инструментов: все элементы стоят на одной высоте, зазор между
// соседями один (kit), разделитель не нарушает ритм, правая часть прижата к
// краю, а высота полосы не зависит от числа элементов.
TEST(UiKit_toolbar_keeps_one_height_one_gap_and_right_edge) {
    KitFrame f;
    ImVec2 mn[4], mx[4];
    float barH = 0.0f, rightLimit = 0.0f;
    auto draw = [&] {
        ictl::BeginToolbar("##bar");
        ictl::ToolbarNext(); ictl::Button(nullptr, "One");
        mn[0] = ImGui::GetItemRectMin(); mx[0] = ImGui::GetItemRectMax();
        ictl::ToolbarNext(); ictl::Button(nullptr, "Two");
        mn[1] = ImGui::GetItemRectMin(); mx[1] = ImGui::GetItemRectMax();
        ictl::ToolbarNext(); ictl::ToolbarSeparator();
        ictl::ToolbarNext(); ictl::Button(nullptr, "Three");
        mn[2] = ImGui::GetItemRectMin(); mx[2] = ImGui::GetItemRectMax();
        const float w = ImGui::CalcTextSize("Right").x + 2.0f * EditorTheme::KitMetrics().ButtonPadX;
        ictl::ToolbarRight(w);
        ictl::Button(nullptr, "Right");
        mn[3] = ImGui::GetItemRectMin(); mx[3] = ImGui::GetItemRectMax();
        rightLimit = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        barH = ImGui::GetWindowSize().y;
        ictl::EndToolbar();
    };
    f.Run(500.0f, draw);
    f.Run(500.0f, draw);   // второй кадр: размеры окна полосы уже известны
    const float gap = EditorTheme::KitMetrics().ToolGap;
    for (int i = 1; i < 4; ++i) CHECK_NEAR(mn[i].y, mn[0].y, 0.5f);
    CHECK_NEAR(mn[1].x - mx[0].x, gap, 0.5f);
    CHECK_NEAR(barH, ictl::ToolbarHeight(), 0.5f);
    CHECK_TRUE(mx[3].x <= rightLimit + 0.5f);
    CHECK_TRUE(mx[3].x >= rightLimit - 2.0f);
}

// ===========================================================================
//  ВЫБОР ЦВЕТА РЕДАКТОРА (editor/src/ui/ColorPicker.h).
//
//  Две части. Математика (hex, HSV, недавние цвета) — обычными вызовами. А
//  поведение палитры проверяется НАСТОЯЩИМИ кадрами ImGui без окна: события
//  мыши кладутся в очередь теми же вызовами, что у бэкенда, и дальше кадр идёт
//  своим ходом. Так проверяется то, ради чего палитра переделана: прозрачность
//  ставится одной полосой, и отмена правок (IsItemActivated /
//  IsItemDeactivatedAfterEdit после поля) видит работу в палитре, хотя та —
//  отдельное окно.
// ===========================================================================
#include "TestFramework.h"

#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "ui/ColorPicker.h"

namespace cp = Sage::UI::color;

TEST(ColorPicker_hex_round_trip) {
    const float rgb[3] = {140 / 255.0f, 88 / 255.0f, 67 / 255.0f};
    const float a = 0.5f;
    CHECK_EQ(cp::ToHex(rgb), std::string("#8C5843"));
    CHECK_EQ(cp::ToHex(rgb, &a), std::string("#8C584380"));

    float out[3] = {0, 0, 0}, outA = 1.0f;
    CHECK_TRUE(cp::ParseHex("#8C584380", out, &outA));
    CHECK_NEAR(out[0], rgb[0], 1e-6f);
    CHECK_NEAR(out[2], rgb[2], 1e-6f);
    CHECK_NEAR(outA, 128 / 255.0f, 1e-6f);

    // Без решётки, коротко, строчными, с хвостовым пробелом.
    CHECK_TRUE(cp::ParseHex("f80 ", out));
    CHECK_NEAR(out[0], 1.0f, 1e-6f);
    CHECK_NEAR(out[1], 136 / 255.0f, 1e-6f);
    CHECK_NEAR(out[2], 0.0f, 1e-6f);

    // Альфа из текста без альфы не трогается.
    outA = 0.25f;
    CHECK_TRUE(cp::ParseHex("#102030", out, &outA));
    CHECK_NEAR(outA, 0.25f, 1e-6f);
}

TEST(ColorPicker_hex_rejects_garbage_and_keeps_colour) {
    float out[3] = {0.1f, 0.2f, 0.3f};
    CHECK_FALSE(cp::ParseHex("#12345", out));
    CHECK_FALSE(cp::ParseHex("#GG0000", out));
    CHECK_FALSE(cp::ParseHex("#FF 00 00", out));
    CHECK_FALSE(cp::ParseHex("", out));
    CHECK_NEAR(out[0], 0.1f, 1e-6f);
    CHECK_NEAR(out[2], 0.3f, 1e-6f);
}

TEST(ColorPicker_grey_keeps_hue_and_black_keeps_saturation) {
    const float prev[3] = {0.6f, 0.8f, 0.5f};
    float hsv[3];
    const float grey[3] = {0.4f, 0.4f, 0.4f};
    cp::RgbToHsvKeep(grey, prev, hsv);
    CHECK_NEAR(hsv[0], 0.6f, 1e-6f);   // тон не прыгнул в красный
    CHECK_NEAR(hsv[1], 0.0f, 1e-6f);
    const float black[3] = {0, 0, 0};
    cp::RgbToHsvKeep(black, prev, hsv);
    CHECK_NEAR(hsv[0], 0.6f, 1e-6f);
    CHECK_NEAR(hsv[1], 0.8f, 1e-6f);   // маркер не уехал в левый край
    CHECK_NEAR(hsv[2], 0.0f, 1e-6f);
}

TEST(ColorPicker_recent_is_unique_newest_first_and_bounded) {
    cp::ClearRecent();
    cp::Remember(ImVec4(1, 0, 0, 1));
    cp::Remember(ImVec4(0, 1, 0, 1));
    cp::Remember(ImVec4(1, 0, 0, 1));
    CHECK_EQ((int)cp::Recent().size(), 2);
    CHECK_NEAR(cp::Recent()[0].x, 1.0f, 1e-6f);
    for (int i = 0; i < 30; ++i) cp::Remember(ImVec4(i / 30.0f, 0.5f, 0.5f, 1));
    CHECK_EQ((int)cp::Recent().size(), cp::kRecentMax);
    cp::ClearRecent();
}

namespace {

// Кадры ImGui без окна и без видеокарты: шрифт собирается в памяти, отрисовка
// никуда не выводится — нужна только логика ввода.
struct Harness {
    ImGuiContext* Ctx = nullptr;
    Harness() {
        Ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 800);
        io.DeltaTime = 1.0f / 60.0f;
        // Нажатие и отпускание кладутся разными кадрами сами — дробить очередь
        // событий незачем.
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    }
    ~Harness() { ImGui::DestroyContext(Ctx); }

    template <class F> void Frame(F&& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(520, 400));
        ImGui::Begin("ColorPickerTest", nullptr, ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }
    void Move(ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); }
    void Button(bool down) { ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down); }
};

} // namespace

TEST(ColorPicker_alpha_bar_sets_opacity_and_reports_one_undo_gesture) {
    cp::ClearRecent();
    Harness h;
    float col[4] = {0.8f, 0.2f, 0.1f, 1.0f};
    bool changed = false, activated = false, deactivatedAfterEdit = false;
    ImVec2 fieldMin, fieldMax;
    auto draw = [&]() {
        ImGui::SetNextItemWidth(240.0f);
        changed = Sage::UI::ColorField4("Tint", col);
        activated = ImGui::IsItemActivated();
        deactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
        fieldMin = ImGui::GetItemRectMin();
        fieldMax = ImGui::GetItemRectMax();
    };
    auto frames = [&](int n) { for (int i = 0; i < n; ++i) h.Frame(draw); };

    frames(3);
    const ImVec2 swatch(fieldMin.x + 20.0f, (fieldMin.y + fieldMax.y) * 0.5f);
    h.Move(swatch);
    frames(1);
    h.Button(true);
    frames(1);
    h.Button(false);
    frames(4);  // окно палитры подбирает размер пару кадров

    const cp::PickerLayout lay = cp::LastPickerLayout();
    CHECK_TRUE(lay.Alpha.Max.x - lay.Alpha.Min.x > 50.0f);   // палитра открыта, полоса есть
    CHECK_TRUE(lay.Alpha.Min.y > fieldMax.y);                // и она под полем

    // Жест по полосе прозрачности: нажали на четверти, протянули до половины.
    h.Move(lay.Alpha.At(0.25f, 0.5f));
    frames(1);
    h.Button(true);
    h.Frame(draw);
    CHECK_TRUE(activated);            // начало жеста видно ПОСЛЕ ПОЛЯ
    CHECK_TRUE(changed);
    CHECK_NEAR(col[3], 0.25f, 0.02f);
    h.Move(lay.Alpha.At(0.5f, 0.5f));
    h.Frame(draw);
    CHECK_FALSE(activated);
    CHECK_NEAR(col[3], 0.5f, 0.02f);
    h.Button(false);
    h.Frame(draw);
    CHECK_TRUE(deactivatedAfterEdit); // и конец — одна запись в истории отмен
    // Цвет прозрачность не трогает.
    CHECK_NEAR(col[0], 0.8f, 1e-4f);
    CHECK_NEAR(col[1], 0.2f, 1e-4f);
    CHECK_NEAR(col[2], 0.1f, 1e-4f);

    // Угол квадрата «насыщенно и ярко» — чистый тон того же цвета.
    frames(1);
    h.Move(lay.SV.At(0.998f, 0.002f));  // сам угол — уже за краем кнопки
    frames(1);
    h.Button(true);
    frames(1);
    h.Button(false);
    frames(1);
    CHECK_NEAR(col[0], 1.0f, 0.01f);
    CHECK_NEAR(col[2], 0.0f, 0.01f);
    CHECK_NEAR(col[3], 0.5f, 0.02f);

    // «Было» возвращает исходный цвет целиком, с прозрачностью.
    const cp::PickerLayout lay2 = cp::LastPickerLayout();
    CHECK_TRUE(lay2.Original.Max.x > lay2.Original.Min.x);
    h.Move(lay2.Original.At(0.5f, 0.5f));
    frames(1);
    h.Button(true);
    frames(1);
    h.Button(false);
    frames(1);
    CHECK_NEAR(col[0], 0.8f, 1e-4f);
    CHECK_NEAR(col[1], 0.2f, 1e-4f);
    CHECK_NEAR(col[3], 1.0f, 1e-4f);

    // Щелчок мимо закрывает палитру, выбранный цвет уходит в недавние.
    h.Move(ImVec2(1200, 780));
    frames(1);
    h.Button(true);
    frames(1);
    h.Button(false);
    frames(2);
    CHECK_FALSE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    CHECK_EQ((int)cp::Recent().size(), 1);
    if (!cp::Recent().empty()) CHECK_NEAR(cp::Recent()[0].x, 0.8f, 1e-4f);
    cp::ClearRecent();
}

// --- ВСТАВКА ЦВЕТА В ЛЮБОЙ ЗАПИСИ ---------------------------------------------
//
// Цвет копируют откуда угодно: из графического редактора (hex), из CSS
// («rgb(…)»), из чужого движка (доли через запятую). Поле обязано понять все.
TEST(ColorPicker_paste_understands_hex_css_and_numbers) {
    float c[3] = {0, 0, 0};
    float a = 1.0f;
    CHECK_TRUE(cp::ParseAny("rgb(255, 128, 0)", c));
    CHECK_NEAR(c[0], 1.0f, 1e-3f);
    CHECK_NEAR(c[1], 128.0f / 255.0f, 1e-3f);
    CHECK_NEAR(c[2], 0.0f, 1e-3f);
    CHECK_TRUE(cp::ParseAny("rgba(0, 0, 255, 0.5)", c, &a));
    CHECK_NEAR(c[2], 1.0f, 1e-3f);
    CHECK_NEAR(a, 0.5f, 1e-3f);           // альфа в CSS — доля
    CHECK_TRUE(cp::ParseAny("0.25, 0.5, 1.0", c));
    CHECK_NEAR(c[0], 0.25f, 1e-4f);
    CHECK_TRUE(cp::ParseAny("10 20 30", c));   // байты через пробел
    CHECK_NEAR(c[2], 30.0f / 255.0f, 1e-4f);
    CHECK_TRUE(cp::ParseAny("#00FF00", c));
    CHECK_NEAR(c[1], 1.0f, 1e-4f);
    // Не цвет — отказ, и цвет не тронут.
    c[0] = 0.7f;
    CHECK_FALSE(cp::ParseAny("hello", c));
    CHECK_FALSE(cp::ParseAny("1, 2", c));
    CHECK_FALSE(cp::ParseAny("300, 0, 0", c));
    CHECK_NEAR(c[0], 0.7f, 1e-4f);
    // «Копировать как…» читается обратно тем же разбором.
    const float src[3] = {0.2f, 0.4f, 0.6f};
    const float srcA = 0.5f;
    float back[3], backA = 0.0f;
    CHECK_TRUE(cp::ParseAny(cp::ToRgbText(src, &srcA).c_str(), back, &backA));
    CHECK_NEAR(back[1], 0.4f, 0.003f);
    CHECK_NEAR(backA, 0.5f, 0.01f);
    CHECK_TRUE(cp::ParseAny(cp::ToFloatText(src).c_str(), back));
    CHECK_NEAR(back[2], 0.6f, 0.001f);
}

// ПКМ -> «Вставить цвет»: цвет из буфера доходит до поля ОДНОЙ правкой — с
// началом и концом жеста после поля, то есть одной записью в истории отмен.
TEST(ColorPicker_context_menu_paste_is_one_undo_step) {
    cp::ClearRecent();
    Harness h;
    float col[3] = {0.1f, 0.1f, 0.1f};
    bool changed = false, activated = false, deactivatedAfterEdit = false;
    int activations = 0, commits = 0;
    ImVec2 fieldMin, fieldMax;
    auto draw = [&]() {
        ImGui::SetNextItemWidth(240.0f);
        changed = Sage::UI::ColorField3("Tint", col);
        activated = ImGui::IsItemActivated();
        deactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
        if (activated) ++activations;
        if (deactivatedAfterEdit) ++commits;
        fieldMin = ImGui::GetItemRectMin();
        fieldMax = ImGui::GetItemRectMax();
    };
    auto frames = [&](int n) { for (int i = 0; i < n; ++i) h.Frame(draw); };
    frames(3);
    ImGui::SetClipboardText("rgb(255, 128, 0)");
    h.Move(ImVec2(fieldMin.x + 20.0f, (fieldMin.y + fieldMax.y) * 0.5f));
    frames(1);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    frames(1);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    frames(3);
    const cp::MenuLayout menu = cp::LastMenuLayout();
    CHECK_TRUE(menu.Paste.Max.y > menu.Paste.Min.y);   // меню открыто, пункт есть
    activations = commits = 0;
    h.Move(menu.Paste.At(0.3f, 0.5f));
    frames(1);
    h.Button(true);
    frames(1);
    h.Button(false);
    frames(4);
    CHECK_NEAR(col[0], 1.0f, 1e-3f);
    CHECK_NEAR(col[1], 128.0f / 255.0f, 1e-3f);
    CHECK_NEAR(col[2], 0.0f, 1e-3f);
    std::printf("       вставка: начал жеста %d, записей %d\n", activations, commits);
    CHECK_EQ(activations, 1);
    CHECK_EQ(commits, 1);
    cp::ClearRecent();
}

// ПИПЕТКА: цвет под курсором ЗА ОКНОМ берётся у системы (здесь — поддельный
// бэкенд), щелчок кладёт его в поле одной правкой. Кнопку, запустившую
// пипетку, надо сначала отпустить; Esc отменяет, ничего не меняя.
namespace {
bool FakeSample(float rgb[3]) { rgb[0] = 0.2f; rgb[1] = 0.6f; rgb[2] = 0.8f; return true; }
bool FakeNotOver() { return false; }
} // namespace

TEST(ColorPicker_eyedropper_takes_colour_from_the_screen) {
    cp::ClearRecent();
    Sage::UI::eyedropper::Backend fake;
    fake.SampleScreen = &FakeSample;
    fake.OverOwnWindow = &FakeNotOver;
    Sage::UI::eyedropper::SetBackend(fake);
    Harness h;
    float col[3] = {1.0f, 1.0f, 1.0f};
    int commits = 0;
    ImVec2 fieldMin, fieldMax;
    auto draw = [&]() {
        ImGui::SetNextItemWidth(240.0f);
        Sage::UI::ColorField3("Tint", col);
        if (ImGui::IsItemDeactivatedAfterEdit()) ++commits;
        fieldMin = ImGui::GetItemRectMin();
        fieldMax = ImGui::GetItemRectMax();
        Sage::UI::eyedropper::Frame();
    };
    auto frames = [&](int n) { for (int i = 0; i < n; ++i) h.Frame(draw); };
    frames(3);
    h.Move(ImVec2(fieldMin.x + 20.0f, (fieldMin.y + fieldMax.y) * 0.5f));
    frames(1);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    frames(1);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    frames(3);
    const cp::MenuLayout menu = cp::LastMenuLayout();
    h.Move(menu.Pick.At(0.3f, 0.5f));
    frames(1);
    h.Button(true);
    frames(1);
    h.Button(false);      // пункт меню срабатывает на отпускании — пипетка пошла
    frames(2);
    CHECK_TRUE(Sage::UI::eyedropper::Active());
    CHECK_NEAR(col[0], 1.0f, 1e-4f);
    h.Move(ImVec2(900, 700));
    h.Button(true);       // щелчок — взять
    frames(1);
    h.Button(false);
    frames(3);
    CHECK_FALSE(Sage::UI::eyedropper::Active());
    CHECK_NEAR(col[0], 0.2f, 1e-4f);
    CHECK_NEAR(col[1], 0.6f, 1e-4f);
    CHECK_NEAR(col[2], 0.8f, 1e-4f);
    CHECK_EQ(commits, 1);

    // Esc — отмена: пипетка гаснет, цвет прежний.
    Sage::UI::eyedropper::Start(1234);
    frames(1);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    frames(1);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    frames(1);
    CHECK_FALSE(Sage::UI::eyedropper::Active());
    CHECK_NEAR(col[0], 0.2f, 1e-4f);
    Sage::UI::eyedropper::SetBackend({});
    cp::ClearRecent();
}

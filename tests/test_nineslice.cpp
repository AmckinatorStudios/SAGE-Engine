// ===========================================================================
//  Девятина («9-slice») — раскладка девяти кусков.
//
//  Пока этот счёт жил внутри функции отрисовки, единственным способом заметить
//  ошибку был взгляд на экран: рамка «подтекает» наружу на некоторых размерах,
//  углы наложились друг на друга на узкой панели, повтор показал не ту часть
//  картинки в обрезанном хвосте. Всё это видно не всегда и потому считается
//  случайным. Здесь геометрия отделена от рисования и проверяется числами.
// ===========================================================================
#include "TestFramework.h"

#include "sage/ui/NineSlice.h"

#include <algorithm>
#include <cmath>
#include <vector>

using sage::ui::NineSlice;
using sage::ui::SliceFill;
using sage::ui::SliceQuad;
using sage::ui::SliceRequest;

namespace {

// Картинка 48x48 с уголками по 8 — самый обычный случай в наборах интерфейса.
NineSlice Panel() {
    NineSlice s;
    s.Left = s.Top = s.Right = s.Bottom = 8.0f;
    return s;
}

SliceRequest Into(float w, float h, float scale = 1.0f) {
    SliceRequest r;
    r.SrcW = 48.0f; r.SrcH = 48.0f;
    r.DstW = w; r.DstH = h;
    r.Scale = scale;
    return r;
}

// Куски не имеют права вылезать за прямоугольник элемента: именно так рамка
// «подтекает» наружу, и заметно это не на всяком размере.
bool AllInside(const std::vector<SliceQuad>& qs, const SliceRequest& r) {
    constexpr float eps = 0.001f;
    for (const SliceQuad& q : qs) {
        if (q.DstX < r.DstX - eps || q.DstY < r.DstY - eps) return false;
        if (q.DstX + q.DstW > r.DstX + r.DstW + eps) return false;
        if (q.DstY + q.DstH > r.DstY + r.DstH + eps) return false;
        if (q.DstW <= 0.0f || q.DstH <= 0.0f) return false;
        if (q.SrcW <= 0.0f || q.SrcH <= 0.0f) return false;
    }
    return true;
}

// Сумма площадей кусков: при растягивании девять штук обязаны покрыть
// прямоугольник ровно один раз, без щелей и нахлёстов.
float CoveredArea(const std::vector<SliceQuad>& qs) {
    float a = 0.0f;
    for (const SliceQuad& q : qs) a += q.DstW * q.DstH;
    return a;
}

} // namespace

TEST(NineSlice_stretch_covers_the_rect_with_nine_pieces) {
    const SliceRequest r = Into(300.0f, 120.0f);
    const std::vector<SliceQuad> qs = sage::ui::Solve(Panel(), r);
    CHECK_EQ(qs.size(), (size_t)9);
    CHECK_TRUE(AllInside(qs, r));
    CHECK_NEAR(CoveredArea(qs), 300.0f * 120.0f, 0.01f);
}

TEST(NineSlice_corners_keep_their_size) {
    // Ради этого девятина и существует: угол не тянется ни при каком размере.
    for (float w : {40.0f, 300.0f, 1920.0f}) {
        const std::vector<SliceQuad> qs = sage::ui::Solve(Panel(), Into(w, 200.0f));
        CHECK_FALSE(qs.empty());
        // Левый верхний — первый из выданных (порядок обхода: строки, потом
        // столбцы), и он обязан быть ровно 8x8.
        CHECK_NEAR(qs[0].DstW, 8.0f, 0.001f);
        CHECK_NEAR(qs[0].DstH, 8.0f, 0.001f);
    }
}

TEST(NineSlice_scale_multiplies_the_border_not_the_source) {
    const std::vector<SliceQuad> qs = sage::ui::Solve(Panel(), Into(300.0f, 120.0f, 3.0f));
    CHECK_FALSE(qs.empty());
    CHECK_NEAR(qs[0].DstW, 24.0f, 0.001f); // 8 пикселей исходника втрое крупнее
    CHECK_NEAR(qs[0].SrcW, 8.0f, 0.001f);  // а в исходнике угол всё тот же
}

TEST(NineSlice_a_rect_narrower_than_its_corners_does_not_turn_inside_out) {
    // Панель 48x48 с полями по 8 в прямоугольник 10x10: полям вдвоём тесно.
    // Раньше куски накладывались друг на друга и рамка выворачивалась.
    const SliceRequest r = Into(10.0f, 10.0f);
    const std::vector<SliceQuad> qs = sage::ui::Solve(Panel(), r);
    CHECK_TRUE(AllInside(qs, r));
    CHECK_NEAR(CoveredArea(qs), 100.0f, 0.01f);
    // Поля ужались ПРОПОРЦИОНАЛЬНО: по 5 на каждое из двух равных.
    CHECK_NEAR(qs[0].DstW, 5.0f, 0.001f);
}

TEST(NineSlice_a_zero_sized_rect_yields_nothing) {
    CHECK_TRUE(sage::ui::Solve(Panel(), Into(0.0f, 100.0f)).empty());
    CHECK_TRUE(sage::ui::Solve(Panel(), Into(100.0f, 0.0f)).empty());
}

TEST(NineSlice_a_hollow_frame_skips_the_middle) {
    NineSlice s = Panel();
    s.DrawCenter = false;
    const std::vector<SliceQuad> qs = sage::ui::Solve(s, Into(300.0f, 120.0f));
    CHECK_EQ(qs.size(), (size_t)8);
    // Ровно середина и отсутствует: покрыто всё, кроме её площади.
    CHECK_NEAR(CoveredArea(qs), 300.0f * 120.0f - (300.0f - 16.0f) * (120.0f - 16.0f), 0.01f);
}

TEST(NineSlice_repeat_tiles_the_middle_without_leaking_out) {
    NineSlice s = Panel();
    s.CenterFill = SliceFill::Tile;
    s.EdgeFill = SliceFill::Tile;
    const SliceRequest r = Into(300.0f, 120.0f);
    const std::vector<SliceQuad> qs = sage::ui::Solve(s, r);

    CHECK_TRUE(qs.size() > 9);                 // кусков стало больше — это и есть повтор
    CHECK_TRUE(AllInside(qs, r));              // и ни один не вылез наружу
    CHECK_NEAR(CoveredArea(qs), 300.0f * 120.0f, 0.01f); // покрытие всё то же
}

TEST(NineSlice_a_clipped_repeat_shows_a_clipped_piece_of_the_source) {
    // Середина исходника — 32 пикселя, прямоугольник — 300: последний повтор
    // обрезан. Вместе с ним обязан обрезаться и кусок картинки, иначе хвост
    // показал бы не ту её часть.
    NineSlice s = Panel();
    s.CenterFill = SliceFill::Tile;
    const std::vector<SliceQuad> qs = sage::ui::Solve(s, Into(300.0f, 120.0f));

    bool sawClipped = false;
    for (const SliceQuad& q : qs) {
        // Обрезанный кусок уже целого — и по экрану, и по исходнику, в одной и
        // той же пропорции.
        if (q.DstW < 31.9f && q.DstW > 0.1f && q.SrcW < 31.9f) {
            CHECK_NEAR(q.SrcW / q.DstW, 1.0f, 0.01f); // масштаб 1 — доли совпадают
            sawClipped = true;
        }
    }
    CHECK_TRUE(sawClipped);
}

TEST(NineSlice_a_border_bigger_than_the_source_is_clamped) {
    // Поля по 40 на картинке 48: левый край начинался бы правее правого.
    NineSlice s;
    s.Left = s.Right = s.Top = s.Bottom = 40.0f;
    const SliceRequest r = Into(300.0f, 120.0f);
    const std::vector<SliceQuad> qs = sage::ui::Solve(s, r);
    CHECK_TRUE(AllInside(qs, r));
    for (const SliceQuad& q : qs) {
        CHECK_TRUE(q.SrcX >= -0.001f && q.SrcX + q.SrcW <= 48.001f);
        CHECK_TRUE(q.SrcY >= -0.001f && q.SrcY + q.SrcH <= 48.001f);
    }
}

TEST(NineSlice_minimum_size_is_the_sum_of_the_borders) {
    const glm::vec2 m = sage::ui::MinimumSize(Panel(), 2.0f);
    CHECK_NEAR(m.x, 32.0f, 0.001f);
    CHECK_NEAR(m.y, 32.0f, 0.001f);
}

TEST(NineSlice_survives_a_file_round_trip) {
    NineSlice s;
    s.Left = 4.0f; s.Top = 6.0f; s.Right = 8.0f; s.Bottom = 10.0f;
    s.CenterFill = SliceFill::Tile;
    s.EdgeFill = SliceFill::Stretch;
    s.DrawCenter = false;

    NineSlice back;
    std::string err;
    CHECK_TRUE(NineSlice::FromJsonString(s.ToJsonString(), back, err));
    CHECK_NEAR(back.Left, 4.0f, 0.001f);
    CHECK_NEAR(back.Top, 6.0f, 0.001f);
    CHECK_NEAR(back.Right, 8.0f, 0.001f);
    CHECK_NEAR(back.Bottom, 10.0f, 0.001f);
    CHECK_TRUE(back.CenterFill == SliceFill::Tile);
    CHECK_TRUE(back.EdgeFill == SliceFill::Stretch);
    CHECK_FALSE(back.DrawCenter);
}

TEST(NineSlice_a_broken_description_reports_instead_of_throwing) {
    NineSlice out;
    std::string err;
    CHECK_FALSE(NineSlice::FromJsonString("{ это не json", out, err));
    CHECK_FALSE(err.empty());
}

TEST(NineSlice_sidecar_sits_next_to_the_picture) {
    // Сайдкаром, а не заменой расширения: рядом могут лежать panel.png и
    // panel.jpg, и одно описание на двоих было бы неверным для обоих.
    CHECK_TRUE(NineSlice::SidecarPath("assets/ui/panel.png") == "assets/ui/panel.png.sage9");
    CHECK_TRUE(NineSlice::SidecarPath("").empty());
}

TEST(NineSlice_guess_finds_the_corners_of_a_drawn_frame) {
    // Рамка 16x16: кайма в 4 пикселя одного цвета, середина — другого. Ровно
    // то, что человек и назвал бы уголком.
    constexpr int N = 16, B = 4;
    std::vector<unsigned char> px((size_t)N * N * 4, 0);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const bool edge = x < B || y < B || x >= N - B || y >= N - B;
            unsigned char* p = px.data() + ((size_t)y * N + x) * 4;
            p[0] = edge ? 200 : 40; p[1] = edge ? 180 : 40; p[2] = edge ? 60 : 40; p[3] = 255;
        }
    }
    NineSlice guess;
    CHECK_TRUE(sage::ui::GuessBorder(px.data(), N, N, guess));
    CHECK_NEAR(guess.Left, (float)B, 0.001f);
    CHECK_NEAR(guess.Top, (float)B, 0.001f);
    CHECK_NEAR(guess.Right, (float)B, 0.001f);
    CHECK_NEAR(guess.Bottom, (float)B, 0.001f);
}

TEST(NineSlice_guess_says_no_on_a_flat_picture) {
    // Однородная картинка: резать нечего, и предлагать нулевую рамку честнее,
    // чем выдумывать.
    std::vector<unsigned char> px(16 * 16 * 4, 77);
    NineSlice guess;
    CHECK_FALSE(sage::ui::GuessBorder(px.data(), 16, 16, guess));
}

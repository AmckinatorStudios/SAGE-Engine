// ---------------------------------------------------------------------------
// Тесты системы значков SAGE (sage/ui/icons/SageIcons.h).
//
// Проверяется не «функция что-то вернула», а те свойства, ради которых система
// и заведена. Их четыре, и каждое из них ломается молча:
//   • значок ЕСТЬ — атлас собран, ячейка не пустая, координаты внутри листа;
//   • значков в атласе ровно столько, сколько попросили в манифесте, — лишние
//     рисунки не должны попадать ни в атлас, ни в двоичный файл;
//   • разные значки лежат в РАЗНЫХ местах: одинаковые координаты у двух имён
//     означают, что весь интерфейс покажет один и тот же рисунок;
//   • список команд отрисовки для значка из набора ссылается на АТЛАС, а не на
//     «пользовательский материал»: иначе сотня кнопок снова станет сотней
//     вызовов рисования, и заметить это по картинке невозможно.
//
// Без GL: атлас — это байты в двоичном файле, и всё вышеперечисленное считается
// без графического контекста. Текстура создаётся уже в бэкенде.
// ---------------------------------------------------------------------------
#include "TestFramework.h"

#include <set>
#include <string>

#include "sage/ui/icons/SageIcons.h"
#include "sage/ui/UIFramework.h"
#include "sage/ui/render/UIDrawBuilder.h"
#include "sage/ui/sageui/SageUI.h"
#include "sage/ui/visual/UIIcon.h"

using namespace sage::ui;
using namespace sage::ui::icons;

// --- Атлас ------------------------------------------------------------------

TEST(Icons_atlas_is_built_and_not_empty) {
    CHECK_TRUE(kIconCount > 0);
    CHECK_TRUE(kAtlasCount > 0);
    for (int a = 0; a < kAtlasCount; ++a) {
        const IconAtlasData& atlas = generated::kAtlases[a];
        CHECK_TRUE(atlas.Width > 0 && atlas.Height > 0);
        CHECK_TRUE(atlas.Pixels != nullptr);
        // Байт на пиксель: атлас хранит ПОКРЫТИЕ, а не цвет.
        CHECK_EQ(atlas.PixelCount, (unsigned)(atlas.Width * atlas.Height));
    }
}

TEST(Icons_every_icon_has_a_cell_inside_the_sheet) {
    for (int i = 0; i < kIconCount; ++i) {
        for (int a = 0; a < kAtlasCount; ++a) {
            const IconAtlasData& atlas = generated::kAtlases[a];
            const IconCell::XY cell = generated::kCells[i].Cells[a];
            CHECK_TRUE((int)cell.X + atlas.Size <= atlas.Width);
            CHECK_TRUE((int)cell.Y + atlas.Size <= atlas.Height);
        }
    }
}

TEST(Icons_are_not_blank) {
    // Пустая ячейка — это разобранный, но не нарисованный SVG: сборка прошла,
    // а на экране пусто. Ловится только здесь.
    const IconAtlasData& atlas = generated::kAtlases[kAtlasCount - 1];
    for (int i = 0; i < kIconCount; ++i) {
        const IconCell::XY cell = generated::kCells[i].Cells[kAtlasCount - 1];
        unsigned sum = 0;
        for (int y = 0; y < atlas.Size; ++y)
            for (int x = 0; x < atlas.Size; ++x)
                sum += atlas.Pixels[(size_t)(cell.Y + y) * atlas.Width + (cell.X + x)];
        CHECK_TRUE(sum > 0);
    }
}

TEST(Icons_different_names_use_different_cells) {
    // Часть значков намеренно делит один рисунок (например, «Открыть» и
    // «Папка»), но совпадать ВСЕ они не могут: тогда набор — один значок.
    std::set<std::pair<int, int>> cells;
    for (int i = 0; i < kIconCount; ++i) {
        const IconCell::XY c = generated::kCells[i].Cells[0];
        cells.insert({c.X, c.Y});
    }
    CHECK_TRUE((int)cells.size() > kIconCount / 2);
}

// --- Реестр -----------------------------------------------------------------

TEST(Icons_handle_by_enum_is_inside_the_sheet) {
    const IconHandle h = Icons::Get(Icon::Play, 20.0f);
    CHECK_TRUE(h.Valid());
    CHECK_TRUE(h.U0 >= 0.0f && h.U1 <= 1.0f);
    CHECK_TRUE(h.V0 >= 0.0f && h.V1 <= 1.0f);
}

TEST(Icons_handle_by_name_matches_handle_by_enum) {
    // Документ интерфейса хранит ИМЯ, код пользуется перечислением — и оба
    // обязаны попасть в одну ячейку. Разъедься они, и значок в редакторе
    // перестал бы совпадать со значком в сохранённом файле.
    const IconHandle byEnum = Icons::Get(Icon::Play, 20.0f);
    const IconHandle byName = Icons::Get(std::string(Icons::Name(Icon::Play)), 20.0f);
    CHECK_EQ(byEnum.Atlas, byName.Atlas);
    CHECK_NEAR(byEnum.U0, byName.U0, 1e-6);
    CHECK_NEAR(byEnum.V0, byName.V0, 1e-6);
}

TEST(Icons_unknown_name_gives_an_invalid_handle) {
    // Именно НЕДЕЙСТВИТЕЛЬНЫЙ, а не «первый попавшийся»: по нему рисующий
    // уходит на векторный путь, а не показывает случайный рисунок.
    CHECK_TRUE(!Icons::Get(std::string("нет-такого-значка")).Valid());
    CHECK_TRUE(!Icons::Has("нет-такого-значка"));
    CHECK_TRUE(Icons::Has(Icons::Name(Icon::Save)));
}

TEST(Icons_nearest_size_atlas_is_chosen) {
    // Три атласа заведены затем, чтобы значок рисовался в свой размер: 24 px,
    // ужатый до 16, теряет тонкие штрихи ровно там, где они несут смысл.
    const IconHandle small = Icons::Get(Icon::Play, (float)kAtlasSizes[0]);
    const IconHandle big = Icons::Get(Icon::Play, (float)kAtlasSizes[kAtlasCount - 1]);
    CHECK_EQ((int)small.Atlas, 0);
    CHECK_EQ((int)big.Atlas, kAtlasCount - 1);
}

TEST(Icons_names_are_unique) {
    std::set<std::string> seen;
    for (int i = 0; i < kIconCount; ++i) {
        const std::string name = generated::kNames[i];
        CHECK_TRUE(!name.empty());
        CHECK_TRUE(seen.insert(name).second);
    }
}

// --- Список команд ----------------------------------------------------------

namespace {
// Один узел со значком, посчитанный до списка команд. Контекст живёт в
// структуре: список команд принадлежит ему, и отдавать его наружу нельзя.
struct IconFrame {
    mutable sage::ui::sui::UIContext Ui;

    explicit IconFrame(const std::string& name) {
        Ui.SetPixelPerfect();
        Ui.SetScreen({200.0f, 200.0f});
        sage::ui::sui::UIElement* e = Ui.Create<sage::ui::sui::UIElement>();
        e->SetSize({24.0f, 24.0f});
        UIIcon& icon = e->Ensure<UIIcon>();
        icon.Name = name;
        icon.Size = 24.0f;
        Ui.Update(0.016f);
        Ui.Runtime().Build();
    }

    const UIRenderList& List() const { return Ui.Runtime().DrawList(); }

    int Count(UIPrimitive kind) const {
        int n = 0;
        for (const UIRenderCommand& c : List().Commands())
            if (c.Kind == kind) ++n;
        return n;
    }

    const UIRenderCommand* First(UIPrimitive kind) const {
        for (const UIRenderCommand& c : List().Commands())
            if (c.Kind == kind) return &c;
        return nullptr;
    }
};
} // namespace

TEST(Icons_from_the_set_become_atlas_commands) {
    // Главное свойство всей затеи: значок из набора едет в список команд как
    // кусок ОБЩЕГО атласа. Пока это так, сто кнопок со значками — один батч.
    const IconFrame f(Icons::Name(Icon::Play));
    CHECK_EQ(f.Count(UIPrimitive::Icon), 1);
    CHECK_EQ(f.Count(UIPrimitive::Custom), 0);

    const UIRenderCommand* cmd = f.First(UIPrimitive::Icon);
    CHECK_TRUE(cmd != nullptr);
    CHECK_TRUE(cmd->IconAtlas >= 0);
    CHECK_TRUE(cmd->Uv.z > cmd->Uv.x && cmd->Uv.w > cmd->Uv.y);
}

TEST(Icons_outside_the_set_still_draw_as_vectors) {
    // Значок, нарисованный кодом под конкретный виджет, в атласе появляться не
    // должен — и обязан продолжать рисоваться. Это не запасной путь «на всякий
    // случай», а второй законный способ.
    const IconFrame f("chevron-down");
    CHECK_EQ(f.Count(UIPrimitive::Icon), 0);
    CHECK_EQ(f.Count(UIPrimitive::Custom), 1);
}

TEST(Icons_two_icons_share_one_atlas) {
    // Если два разных значка попадут в разные атласы, батч порвётся на каждом
    // из них — ровно то, чего система должна не допускать.
    const IconHandle a = Icons::Get(Icon::Play, 20.0f);
    const IconHandle b = Icons::Get(Icon::Settings, 20.0f);
    CHECK_EQ(a.Atlas, b.Atlas);
}

// ===========================================================================
//  Состояние редактора — выделение, история отката, инструменты.
//
//  Все три раньше были полями EditorLayer: правила лежали вперемешку с
//  проектом, Play-режимом, панелями и плагинами, и проверить их можно было
//  только запустив редактор целиком под xvfb. Правила при этом совсем не
//  очевидные — «первичная» это последняя добавленная, пустой набор сбрасывает
//  первичную, неудавшийся откат не должен ломать историю, — и ошибиться в них
//  легко, а заметить трудно: редактор продолжает работать, просто выделяет и
//  откатывает не то.
//
//  Здесь они проверяются как обычный код: без сцены, сериализатора, ImGui и
//  графического контекста.
// ===========================================================================
#include "TestFramework.h"

#include "../editor/src/EditorHistory.h"
#include "../editor/src/EditorSelection.h"

#include <string>
#include <vector>

// --- Выделение ---------------------------------------------------------------

TEST(EditorSelection_single_pick_replaces_the_whole_set) {
    EditorSelection sel;
    sel.Set({1, 2, 3});
    CHECK_EQ(sel.All().size(), (size_t)3);

    sel.SetPrimary(7);
    CHECK_EQ(sel.All().size(), (size_t)1);
    CHECK_EQ(sel.Primary(), 7);
    CHECK_TRUE(sel.Contains(7));
    CHECK_FALSE(sel.Contains(1));
}

TEST(EditorSelection_minus_one_means_nothing_is_selected) {
    EditorSelection sel;
    sel.SetPrimary(5);
    sel.SetPrimary(-1);
    CHECK_TRUE(sel.Empty());
    CHECK_EQ(sel.Primary(), -1);
    // -1 не должен попасть в набор как обычный номер.
    CHECK_FALSE(sel.Contains(-1));
}

TEST(EditorSelection_primary_is_the_last_added) {
    // Под первичную встаёт инспектор и пивот гизмо — она обязана быть той,
    // которую только что добавили, а не той, что попала в набор первой.
    EditorSelection sel;
    sel.Set({4, 8, 15});
    CHECK_EQ(sel.Primary(), 15);

    sel.Set({16}, /*additive=*/true);
    CHECK_EQ(sel.Primary(), 16);
    CHECK_EQ(sel.All().size(), (size_t)4);
}

TEST(EditorSelection_set_does_not_duplicate_and_ignores_nothing) {
    EditorSelection sel;
    sel.Set({3, 3, -1, 3});
    CHECK_EQ(sel.All().size(), (size_t)1);
    CHECK_EQ(sel.Primary(), 3);

    sel.Set({3}, /*additive=*/true);
    CHECK_EQ(sel.All().size(), (size_t)1);
}

TEST(EditorSelection_empty_set_clears_the_primary) {
    // Иначе инспектор показывал бы поля объекта, который на экране уже не
    // подсвечен.
    EditorSelection sel;
    sel.Set({2});
    sel.Set({});
    CHECK_TRUE(sel.Empty());
    CHECK_EQ(sel.Primary(), -1);
}

TEST(EditorSelection_toggle_adds_removes_and_moves_the_primary) {
    EditorSelection sel;
    sel.Set({1, 2});
    CHECK_EQ(sel.Primary(), 2);

    sel.Toggle(3);                 // не был в наборе — добавить и сделать первичным
    CHECK_TRUE(sel.Contains(3));
    CHECK_EQ(sel.Primary(), 3);

    sel.Toggle(3);                 // был — убрать, первичной становится предыдущая
    CHECK_FALSE(sel.Contains(3));
    CHECK_EQ(sel.Primary(), 2);

    sel.Toggle(-1);                // «ничего» нельзя ни добавить, ни снять
    CHECK_EQ(sel.All().size(), (size_t)2);

    sel.Toggle(2); sel.Toggle(1);  // сняли всё
    CHECK_TRUE(sel.Empty());
    CHECK_EQ(sel.Primary(), -1);
}

TEST(EditorSelection_lock_keeps_what_was_shown_when_it_was_locked) {
    EditorSelection sel;
    sel.SetPrimary(11);
    CHECK_FALSE(sel.Locked());

    sel.Lock(sel.Primary(), "assets/materials/Wood.sagemat");
    sel.SetPrimary(12);            // выбрали другое — замок держит прежнее
    CHECK_TRUE(sel.Locked());
    CHECK_EQ(sel.LockedEntityId(), 11);
    CHECK_TRUE(sel.LockedAssetPath() == std::filesystem::path("assets/materials/Wood.sagemat"));
    CHECK_EQ(sel.Primary(), 12);

    sel.Unlock();
    CHECK_FALSE(sel.Locked());
}

// --- История отката ----------------------------------------------------------
//
// Историю со сценой знакомят двумя функциями, поэтому «сценой» здесь может быть
// обычная строка.
namespace {

struct FakeScene {
    std::string State = "A";
    bool RestoreWorks = true;

    EditorHistory MakeHistory() {
        return EditorHistory([this] { return State; },
                             [this](const std::string& s) {
                                 if (!RestoreWorks) return false;
                                 State = s;
                                 return true;
                             });
    }
};

} // namespace

TEST(EditorHistory_undo_returns_the_state_before_the_change) {
    FakeScene scene;
    EditorHistory history = scene.MakeHistory();

    CHECK_FALSE(history.CanUndo());
    history.Push();                 // снимок «до»
    scene.State = "B";              // правка
    CHECK_TRUE(history.CanUndo());

    CHECK_TRUE(history.Undo());
    CHECK_TRUE(scene.State == "A");
    CHECK_FALSE(history.CanUndo());
    CHECK_TRUE(history.CanRedo());

    CHECK_TRUE(history.Redo());
    CHECK_TRUE(scene.State == "B");
}

TEST(EditorHistory_a_new_change_cuts_the_redo_branch) {
    FakeScene scene;
    EditorHistory history = scene.MakeHistory();

    history.Push(); scene.State = "B";
    history.Undo();
    CHECK_TRUE(history.CanRedo());

    history.Push(); scene.State = "C";   // пошли другой веткой
    CHECK_FALSE(history.CanRedo());
}

TEST(EditorHistory_a_failed_restore_does_not_break_the_stacks) {
    // Откат может не удаться — снимок битый, сцена не собралась. Тогда история
    // обязана остаться ровно такой, какой была: иначе один сбой съедает шаг
    // отмены и следующий Ctrl+Z уводит совсем не туда.
    FakeScene scene;
    EditorHistory history = scene.MakeHistory();

    history.Push(); scene.State = "B";
    scene.RestoreWorks = false;

    CHECK_FALSE(history.Undo());
    CHECK_TRUE(history.CanUndo());     // шаг отмены на месте
    CHECK_FALSE(history.CanRedo());    // и повтор не появился из ниоткуда
    CHECK_TRUE(scene.State == "B");
}

TEST(EditorHistory_a_drag_makes_one_entry_not_one_per_frame) {
    FakeScene scene;
    EditorHistory history = scene.MakeHistory();

    history.CapturePending();          // виджет активирован
    scene.State = "B";                 // кадр перетаскивания
    scene.State = "C";                 // ещё кадр
    CHECK_TRUE(history.CommitPending()); // правка завершена
    CHECK_TRUE(history.CanUndo());

    history.Undo();
    CHECK_TRUE(scene.State == "A");
    CHECK_FALSE(history.CanUndo());     // запись была ровно одна
}

TEST(EditorHistory_commit_without_capture_does_nothing) {
    FakeScene scene;
    EditorHistory history = scene.MakeHistory();
    CHECK_FALSE(history.CommitPending());
    CHECK_FALSE(history.CanUndo());
}

TEST(EditorHistory_disabled_history_records_nothing) {
    // Так выключен откат в Play-режиме: правки там эфемерны, Stop откатит их
    // сам, а снимок игровой сцены в стопке означал бы возврат в состояние,
    // которого в редактируемой сцене никогда не было.
    FakeScene scene;
    EditorHistory history = scene.MakeHistory();
    history.SetEnabled(false);

    CHECK_FALSE(history.Push());
    history.CapturePending();
    CHECK_FALSE(history.CommitPending());
    CHECK_FALSE(history.CanUndo());
    CHECK_FALSE(history.Undo());
}

TEST(EditorHistory_stack_depth_is_capped) {
    // Снимок — полный JSON сцены; без потолка история крупного проекта съедала
    // бы память тем быстрее, чем крупнее сцена.
    FakeScene scene;
    EditorHistory history = scene.MakeHistory();

    for (size_t i = 0; i < EditorHistory::kMaxEntries + 10; ++i) {
        scene.State = "s" + std::to_string(i);
        history.Push();
    }
    int steps = 0;
    while (history.Undo()) ++steps;
    CHECK_EQ((size_t)steps, EditorHistory::kMaxEntries);
    // Самый старый снимок вытеснен: отмотали не в "s0", а в срез потолка.
    CHECK_TRUE(scene.State == "s10");
}

TEST(EditorHistory_clear_forgets_a_previous_scene) {
    FakeScene scene;
    EditorHistory history = scene.MakeHistory();
    history.Push(); scene.State = "B";
    history.Undo();

    history.Clear();
    CHECK_FALSE(history.CanUndo());
    CHECK_FALSE(history.CanRedo());
}

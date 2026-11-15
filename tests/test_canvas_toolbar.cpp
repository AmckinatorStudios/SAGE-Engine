// ===========================================================================
//  ПАНЕЛЬ ХОЛСТА ИНТЕРФЕЙСА: что показывать сейчас и чем задан кадр
//  предпросмотра (editor/src/interface/CanvasToolbarRules.h).
// ===========================================================================
#include "TestFramework.h"

#include "../editor/src/interface/CanvasToolbarRules.h"

namespace bar = sage::editor::canvasbar;

TEST(CanvasToolbar_extra_settings_follow_their_feature) {
    UIToolSettings t;
    // Шаг сетки — пока сетку показывают или к ней притягивают.
    t.ShowGrid = false;
    t.Snap.ToGrid = false;
    CHECK_TRUE(!bar::ShowGridStep(t));
    t.Snap.ToGrid = true;
    CHECK_TRUE(bar::ShowGridStep(t));
    t.Snap.ToGrid = false;
    t.ShowGrid = true;
    CHECK_TRUE(bar::ShowGridStep(t));
    // Порог — пока работает хоть одна привязка; от показа сетки не зависит.
    t.Snap.ToGrid = false;
    t.Snap.ToEdges = false;
    CHECK_TRUE(!bar::ShowSnapThreshold(t));
    t.Snap.ToEdges = true;
    CHECK_TRUE(bar::ShowSnapThreshold(t));
    // Цвет подложки — только при подложке; безопасная зона — когда включена.
    t.Backdrop = 0.0f;
    CHECK_TRUE(!bar::ShowBackdropColor(t));
    t.Backdrop = 0.3f;
    CHECK_TRUE(bar::ShowBackdropColor(t));
    t.ShowSafeArea = false;
    CHECK_TRUE(!bar::ShowSafeAreaPercent(t));
    t.ShowSafeArea = true;
    CHECK_TRUE(bar::ShowSafeAreaPercent(t));
    // Скрытие не трогает значения.
    t.Snap.GridStep = 24.0f;
    t.ShowGrid = t.Snap.ToGrid = false;
    CHECK_NEAR(t.Snap.GridStep, 24.0f, 1e-6f);
}

TEST(CanvasToolbar_preview_mode_tells_game_preset_and_custom_apart) {
    UIToolSettings t;
    CHECK_TRUE(bar::ModeOf(t) == bar::PreviewMode::Game);
    CHECK_TRUE(!bar::ShowCustomResolution(t));
    // Готовый размер — пресет: полей W/H нет.
    t.PreviewSize = {1280, 720};
    CHECK_TRUE(bar::ModeOf(t) == bar::PreviewMode::Preset);
    CHECK_TRUE(!bar::ShowCustomResolution(t));
    // Тот же размер, но выбранный как «своё», — своё: поля и поворот есть.
    t.PreviewCustom = true;
    CHECK_TRUE(bar::ModeOf(t) == bar::PreviewMode::Custom);
    CHECK_TRUE(bar::ShowCustomResolution(t));
    // Размер не из списка — своё и без флага (старое состояние).
    t.PreviewCustom = false;
    t.PreviewSize = {800, 600};
    CHECK_TRUE(bar::ModeOf(t) == bar::PreviewMode::Custom);
    // Нулевой размер — как в игре, даже с флагом.
    t.PreviewSize = {0, 0};
    t.PreviewCustom = true;
    CHECK_TRUE(bar::ModeOf(t) == bar::PreviewMode::Game);
}

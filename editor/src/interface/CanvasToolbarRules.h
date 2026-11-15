#pragma once
#include <glm/glm.hpp>

#include "../UIToolSettings.h"

// ---------------------------------------------------------------------------
// ПРАВИЛА ПАНЕЛИ ХОЛСТА — что показывать сейчас. Без ImGui: проверяются
// модульным тестом (tests/test_canvas_toolbar.cpp), панель только спрашивает.
//
// Дополнительная настройка видна, пока работает то, что она настраивает;
// значение при скрытии не трогается и возвращается при включении.
// ---------------------------------------------------------------------------
namespace sage::editor::canvasbar {

// Шаг сетки — общий у показа сетки и притяжки к ней.
inline bool ShowGridStep(const UIToolSettings& t) { return t.ShowGrid || t.Snap.ToGrid; }
// Порог привязки — общий у сетки и краёв.
inline bool ShowSnapThreshold(const UIToolSettings& t) { return t.Snap.ToGrid || t.Snap.ToEdges; }
// Цвет подложки ничего не меняет, пока подложки нет.
inline bool ShowBackdropColor(const UIToolSettings& t) { return t.Backdrop > 0.0f; }
inline bool ShowSafeAreaPercent(const UIToolSettings& t) { return t.ShowSafeArea; }

// Готовые разрешения предпросмотра. Вертикальное рядом с горизонтальными
// нарочно: телефон держат вертикально.
struct PreviewPreset {
    const char* Label;
    int W, H;
};
inline constexpr PreviewPreset kPreviewPresets[] = {
    {"1920 x 1080", 1920, 1080},
    {"1280 x 720", 1280, 720},
    {"1080 x 1920", 1080, 1920},
};

// Чем задан кадр предпросмотра: как в игре, готовым размером или своим.
enum class PreviewMode { Game, Preset, Custom };
inline PreviewMode ModeOf(const UIToolSettings& t) {
    if (t.PreviewSize.x <= 0 || t.PreviewSize.y <= 0) return PreviewMode::Game;
    if (t.PreviewCustom) return PreviewMode::Custom;
    for (const PreviewPreset& p : kPreviewPresets)
        if (t.PreviewSize.x == p.W && t.PreviewSize.y == p.H) return PreviewMode::Preset;
    // Размер не из списка, но и не выбран как «своё» (старое состояние) — это
    // всё-таки своё: показать его поля, а не потерять.
    return PreviewMode::Custom;
}
// Поля W/H и «Повернуть экран» — только у своего разрешения.
inline bool ShowCustomResolution(const UIToolSettings& t) { return ModeOf(t) == PreviewMode::Custom; }

} // namespace sage::editor::canvasbar

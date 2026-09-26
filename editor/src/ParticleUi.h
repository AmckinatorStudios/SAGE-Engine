#pragma once
#include <functional>
#include <string>
#include <vector>

#include "sage/render/ParticleEffect.h"

// ---------------------------------------------------------------------------
// Инспектор эффекта частиц: все модули, кривые и градиенты.
//
// Отдельно от панели инспектора и БЕЗ EditorHost: всё, что требует редактора
// (слоты файлов, отмена), приходит крючками. Поэтому разметку проверяют
// модульные тесты настоящими кадрами ImGui — как PostChainUi: эффект со всеми
// модулями включёнными, мышь проходит по всему окну, и ни в одной точке не
// должно оказаться двух виджетов с одним идентификатором.
//
// Модули — разделы со своим выключателем в заголовке. Выключенный модуль не
// действует, но его настройки остаются: включил обратно — всё на месте.
// ---------------------------------------------------------------------------
namespace sage::editor {

struct ParticleUiHooks {
    std::function<void()> BeforeEdit;   // снимок для отмены перед дискретной правкой
    std::function<void()> TrackItem;    // перетаскивание значения (TrackLastImGuiItem)
    // Слот картинки / файла эффекта. true — путь изменился.
    std::function<bool(const char* id, std::string& path)> TextureSlot;
    std::function<bool(const char* id, std::string& path)> EffectSlot;
    // Существует ли файл по ссылке проекта (для поиска кадров по номерам).
    std::function<bool(const std::string& ref)> FileExists;
};

// Весь эффект. true — что-то изменилось.
bool DrawParticleEffect(sage::fx::ParticleEffect& fx, const ParticleUiHooks& hooks);

// Кривая на [0, 1] со значениями в [lo, hi] (диапазон расширяется, если ключи
// за него вышли). Двойной щелчок — ключ, правый — удалить, тянуть — двигать.
bool DrawCurve(const char* label, sage::fx::Curve& curve, float lo, float hi,
               const ParticleUiHooks& hooks);

// Градиент: сверху — ключи прозрачности, снизу — ключи цвета.
bool DrawGradient(const char* label, sage::fx::Gradient& gradient, const ParticleUiHooks& hooks);

// Кадры по номеру: из «smoke_3.png» находит smoke_0.png, smoke_1.png, … подряд,
// пока файлы существуют. Нумерация без ведущих нулей и с ними (smoke_03).
// Пусто — в имени нет номера.
std::vector<std::string> NumberedSequence(const std::string& anyFrame,
                                          const std::function<bool(const std::string&)>& exists);

} // namespace sage::editor

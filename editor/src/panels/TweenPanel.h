#pragma once
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "sage/anim/Tween.h"

class EditorHost;
class Scene;

// ---------------------------------------------------------------------------
// ОКНО TWEEN — быстрое движение без клипа.
//
// Выбрал объект → «Создать твин» → свойство → конечное значение → Play. Всё
// на одном экране: строки свойств (откуда, куда, кривая) и справа — их полосы
// на шкале времени; полосу тянут за середину (когда начать), за края (сколько
// длится), конец шкалы — растянуть весь твин. Последовательность и параллель
// — это просто положение полос, и кнопки «По очереди» / «Вместе» расставляют
// их одним щелчком.
//
// ПРАВИТ ДАННЫЕ, А НЕ ИГРАЕТ САМ. Окно меняет TweenComponent объекта (тот же,
// что лежит в сцене и играет в игре), а Play зовёт ОБЩИЙ проигрыватель сцены
// (Scene::Tweens) — тот же, что и Tween.to из Lua. Своего «редакторского»
// проигрывания нет, поэтому показанное здесь и есть то, что будет в игре.
//
// ПРОСМОТР ОБРАТИМ. Перед Play значения свойств запоминаются, Reset (а также
// любая правка, смена объекта, запуск игры и сохранение) их возвращает: иначе
// посмотренный твин оставался бы в сцене и уезжал в файл.
// ---------------------------------------------------------------------------
class TweenPanel {
public:
    void Draw(EditorHost& host, bool* open, const std::string& windowId);
    // Вернуть объекту значения до просмотра. Зовёт редактор перед Play и
    // сохранением сцены.
    void StopPreview(EditorHost& host);
    // Открыть твин объекта (из инспектора «Изменить в окне Tween»).
    void Open(int objectId, int tweenIndex) {
        m_ownerId = objectId;
        m_index = tweenIndex;
        m_follow = false;
        m_focusFrames = 3;
    }

    // Для самопроверки.
    bool Previewing() const { return m_previewing; }
    int OwnerId() const { return m_ownerId; }
    int TweenIndex() const { return m_index; }
    // Сыграть/поставить на паузу/сбросить — то же, что кнопки окна.
    void Play(EditorHost& host);
    void Pause(EditorHost& host);
    void Reset(EditorHost& host) { StopPreview(host); }
    // Время просмотра, секунды.
    float PreviewTime(EditorHost& host) const;

private:
    struct Saved {
        entt::entity Entity = entt::null;
        std::string Property;
        glm::vec4 Value{0.0f};
    };

    sage::anim::TweenComponent* Component(EditorHost& host);
    sage::anim::TweenClip* Clip(EditorHost& host);
    void Edited(EditorHost& host);   // правка: вернуть просмотр и записать отмену

    void DrawHeader(EditorHost& host, sage::anim::TweenComponent& tc);
    void DrawSettings(EditorHost& host, sage::anim::TweenClip& clip);
    void DrawTracks(EditorHost& host, sage::anim::TweenClip& clip, entt::entity target);
    bool DrawValue(const char* id, const sage::anim::PropertyType* p, glm::vec4& v);
    void DrawTimelineRow(EditorHost& host, sage::anim::TweenClip& clip, size_t index, float width);
    void DrawRuler(EditorHost& host, sage::anim::TweenClip& clip, float width);
    void DrawCurveEditor(EditorHost& host, sage::anim::TweenTrack& track);

    // Шкала: пикселей в секунде и начало по x (обновляются каждый кадр).
    float m_pxPerSec = 100.0f;
    float m_visibleSeconds = 1.0f;

    int m_ownerId = 0;         // объект, чьи твины правятся
    int m_index = 0;           // какой твин
    bool m_follow = true;      // следовать за выбором в сцене
    int m_selectedTrack = 0;   // чья кривая в редакторе кривой
    int m_focusFrames = 0;

    // Просмотр.
    bool m_previewing = false;
    bool m_paused = false;
    sage::anim::TweenHandle m_handle = sage::anim::kNoTween;
    std::vector<Saved> m_saved;
    const Scene* m_previewScene = nullptr;
    float m_lastTime = 0.0f;   // где стоял бегунок, когда твин кончился
    bool m_dragging = false;   // тянут полосу или ручку кривой
};

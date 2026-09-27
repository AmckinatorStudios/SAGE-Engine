#pragma once
#include <set>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "sage/anim/Tween.h"

class EditorHost;
class Scene;

// ---------------------------------------------------------------------------
// ОКНО TWEEN — «объект → свойства → время → кривая».
//
// ПРОСТОЕ — ПРОСТО. Выбрал объект → «+ Tween» → свойство → поправил конечное
// значение. Твин создаётся сразу рабочим: начало — нынешнее значение, конец —
// оно же, полсекунды, плавное замедление; править остаётся одно число.
//
// ОДНА РАБОЧАЯ ОБЛАСТЬ. Каждое свойство — карточка: название, «из → в»,
// кривая человеческими словами и полоса на общей шкале времени (тянется за
// середину и за точки начала и конца). Больше нигде эти настройки не
// повторяются: инспектор показывает только сводку, а редактор кривой
// открывается по просьбе, а не висит панелью.
//
// СЛОЖНОЕ — СПРЯТАНО. Порядок (по очереди / вместе / пауза), задержка, повтор,
// скорость, направление, что после, другой объект, имя для скриптов — в
// свёрнутом «Дополнительно». Большинству твинов оно не нужно вовсе.
//
// ПРАВИТ ДАННЫЕ, А НЕ ИГРАЕТ САМ. Окно меняет TweenComponent объекта, а
// просмотр зовёт ОБЩИЙ проигрыватель сцены (Scene::Tweens) — тот же, что
// Tween.to из Lua: показанное здесь и есть то, что будет в игре.
//
// ПРОСМОТР ОБРАТИМ. Перед просмотром значения свойств запоминаются, «Вернуть»
// (а также любая правка, смена объекта, запуск игры и сохранение) их
// возвращает: иначе посмотренный твин оставался бы в сцене и уезжал в файл.
// ---------------------------------------------------------------------------

// То, что окну нужно и снаружи (инспектор, самопроверка).
namespace tweenui {

// Свойство в меню «+ Добавить свойство»: только то, что у объекта ЕСТЬ, и
// человеческим названием. Section — раздел меню (Transform, Rendering,
// Interface, Camera, Audio); пусто — редкое, уходит под «Ещё».
struct Choice {
    const sage::anim::PropertyType* Property = nullptr;
    const char* Section = "";
    std::string Title;   // уже переведено
};
std::vector<Choice> Choices(const entt::registry& reg, entt::entity e);

// Название свойства для человека («Цвет текста», а не «label.color»).
std::string TitleOf(const std::string& propertyId);
// Название кривой для человека («Плавное замедление», а не «quad-out»).
std::string EaseName(const sage::anim::Ease& e);
// Сводка твина для инспектора: «2 свойства · 1.6 с».
std::string Summary(const sage::anim::TweenClip& clip);

} // namespace tweenui

class TweenPanel {
public:
    // Что сделать, открыв окно (кнопки инспектора).
    enum class Action { None, AddProperty, Play };

    void Draw(EditorHost& host, bool* open, const std::string& windowId);
    // Вернуть объекту значения до просмотра. Зовёт редактор перед Play и
    // сохранением сцены.
    void StopPreview(EditorHost& host);
    void Open(int objectId, int tweenIndex, Action action = Action::None);

    // Быстрое создание: свойство в нынешний твин объекта (нет твина — заводит
    // его). Начало — нынешнее значение, конец — оно же, 0.5 с, плавное
    // замедление. true — добавлено.
    bool AddProperty(EditorHost& host, const std::string& propertyId);

    // Для самопроверки и инспектора.
    bool Previewing() const { return m_previewing; }
    int OwnerId() const { return m_ownerId; }
    int TweenIndex() const { return m_index; }
    void Play(EditorHost& host);
    void Pause(EditorHost& host);
    void Reset(EditorHost& host) { StopPreview(host); }
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
    void Select(int tweenIndex);
    // Значение свойства «как есть» — во время просмотра то, что было ДО него.
    glm::vec4 Current(entt::registry& reg, entt::entity target, const sage::anim::PropertyType& p) const;

    void DrawHeader(EditorHost& host, sage::anim::TweenComponent& tc);
    void DrawEmpty(EditorHost& host, bool hasTween);
    void DrawRuler(EditorHost& host, sage::anim::TweenClip& clip);
    void DrawCard(EditorHost& host, sage::anim::TweenClip& clip, size_t index, entt::entity target);
    void DrawLane(EditorHost& host, sage::anim::TweenClip& clip, size_t index, float y0, float y1);
    void DrawAdvanced(EditorHost& host, sage::anim::TweenComponent& tc, sage::anim::TweenClip& clip);
    bool DrawValue(const char* id, const sage::anim::PropertyType* p, glm::vec4& v, float width, bool uniform);

    // Всплывающие окна открываются на уровне окна, а не внутри карточки: у
    // OpenPopup и BeginPopup должен совпасть весь стек ID, а карточка его
    // меняет (PushID). Карточка только ставит запрос.
    void DrawPopups(EditorHost& host, sage::anim::TweenClip* clip, entt::entity target);
    void DrawPropertyMenu(EditorHost& host, entt::entity target);
    void DrawEasePicker(EditorHost& host, sage::anim::TweenClip& clip);
    void DrawCurveEditor(EditorHost& host, sage::anim::TweenClip& clip);
    void DrawCardMenu(EditorHost& host, sage::anim::TweenClip& clip, entt::entity target);
    void EasePickerBody(EditorHost& host, sage::anim::TweenClip& clip);
    void CurveEditorBody(EditorHost& host, sage::anim::TweenClip& clip);
    void CardMenuBody(EditorHost& host, sage::anim::TweenClip& clip, entt::entity target);

    // Раскладка (считается каждый кадр): левая колонка карточек и шкала.
    float m_infoW = 360.0f;
    float m_laneX = 0.0f;      // экранный x начала шкалы
    float m_laneW = 100.0f;
    float m_pxPerSec = 100.0f;
    float m_rulerTop = 0.0f;
    float m_cardsBottom = 0.0f;

    int m_ownerId = 0;         // объект, чьи твины правятся
    int m_index = 0;           // какой твин
    int m_seenSelection = -1;  // выбор в сцене, который окно уже видело
    int m_focusFrames = 0;
    bool m_advanced = false;   // «Дополнительно» раскрыто
    std::set<int> m_splitAxes; // дорожки масштаба, где оси правят по отдельности

    // Запросы всплывающих окон (см. DrawPopups).
    bool m_wantProps = false;
    bool m_wantEase = false;
    bool m_wantCurve = false;
    bool m_wantCardMenu = false;
    bool m_wantPlay = false;
    int m_popupTrack = 0;

    // Просмотр.
    bool m_previewing = false;
    bool m_paused = false;
    sage::anim::TweenHandle m_handle = sage::anim::kNoTween;
    std::vector<Saved> m_saved;
    const Scene* m_previewScene = nullptr;
    float m_lastTime = 0.0f;   // где стоял бегунок, когда твин кончился
};

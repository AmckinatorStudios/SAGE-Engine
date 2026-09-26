#pragma once
// ---------------------------------------------------------------------------
// Бегунок проигрывателя: что делать со звуком, пока его тянут.
//
// ПОЧЕМУ ОТДЕЛЬНО. Бегунок перематывал звук в КАЖДОМ кадре перетаскивания, а
// звук при этом продолжал играть. Каждая перемотка у звуковой подсистемы
// отложенная (её исполняет звуковой поток), и курсор, спрошенный в следующем
// кадре, ещё старый — бегунок прыгал назад, перематывал снова, и в динамиках
// получалась каша из обрывков, «будто играет одновременно в нескольких местах».
//
// Правильно — как в любом проигрывателе: схватили бегунок — звук на паузе,
// тянут — двигается только бегунок, отпустили — ОДНА перемотка и продолжение,
// если до этого играло. Логика вынесена сюда, чтобы её можно было проверить
// без звуковой карты (на машине сборки её нет).
// ---------------------------------------------------------------------------

namespace sage::audio {

struct ScrubState {
    bool Dragging = false;
    bool ResumeAfter = false;   // играло ли до того, как схватили
    float Target = 0.0f;        // куда тянут; показывается вместо курсора звука
};

// Что проигрывателю сделать со звуком в этом кадре.
struct ScrubAction {
    bool Pause = false;
    bool Seek = false;
    float SeekTo = 0.0f;
    bool Resume = false;
};

// Кадр бегунка. active — бегунок держат (ImGui::IsItemActive), value — где он
// сейчас, playing — играл ли звук в начале кадра.
inline ScrubAction StepScrub(ScrubState& s, bool active, float value, bool playing) {
    ScrubAction a;
    if (active) {
        if (!s.Dragging) {
            s.Dragging = true;
            s.ResumeAfter = playing;
            a.Pause = playing;
        }
        s.Target = value;
        return a;
    }
    if (s.Dragging) {
        s.Dragging = false;
        a.Seek = true;
        a.SeekTo = s.Target < 0.0f ? 0.0f : s.Target;
        a.Resume = s.ResumeAfter;
        s.ResumeAfter = false;
    }
    return a;
}

} // namespace sage::audio

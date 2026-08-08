#pragma once

class EditorHost;

// Панель Game — «игровое окно»: изображение сцены от ИГРОВОЙ камеры (первая
// сущность с CameraComponent.Primary), а не от редакторской. Табом делит
// центр с Viewport; при входе в Play редактор вызывает RequestFocus() — таб
// Game выходит на передний план, как в привычных редакторах.
class GamePanel {
public:
    void Draw(EditorHost& host, bool* open);

    // Запросить фокус панели (вызывается при StartPlay). Держим запрос
    // несколько кадров: одноразовый SetNextWindowFocus проигрывает фокусу
    // Viewport, который выставляет построение dock-раскладки в тот же кадр.
    void RequestFocus() { m_focusFrames = 3; }

    // Панель в фокусе (последний нарисованный кадр). По этому флагу редактор
    // решает, отдавать ли ввод ИГРЕ: играть в Play-режиме можно только когда
    // человек смотрит в игровое окно, иначе WASD уезжал бы в игру прямо во
    // время правки сцены.
    bool Focused() const { return m_focused; }

private:
    int m_focusFrames = 0;
    bool m_focused = false;
};

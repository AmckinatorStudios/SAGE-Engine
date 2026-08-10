#pragma once

#include <string>

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

    // --- Ввод для ИНТЕРФЕЙСА игры ------------------------------------------
    //
    // Интерфейс игры (кнопки меню, слоты инвентаря) в
    // Play-режиме редактора не работал вовсе: сцена его РИСОВАЛА, но
    // sage::ui::UpdateSceneUI не звал никто — этот вызов был только в плеере.
    // Со стороны человека это выглядело так: в собранной игре меню кликается, в
    // редакторе — нет, и понять, чья это поломка (игры или движка), неоткуда.
    //
    // Курсор при этом живёт в координатах ОКНА РЕДАКТОРА, а интерфейс — в
    // координатах игрового кадра, и знает про перевод между ними только сама
    // панель: она одна знает, где нарисована её картинка.
    bool MouseInside() const { return m_mouseInside; }
    float MouseX() const { return m_mouseX; }
    float MouseY() const { return m_mouseY; }
    bool MouseDown() const { return m_mouseDown; }
    // Набранный за кадр текст (UTF-8) — для полей ввода в интерфейсе игры.
    const std::string& TypedText() const { return m_typed; }

private:
    int m_focusFrames = 0;
    bool m_focused = false;
    // Положение курсора В ПИКСЕЛЯХ ИГРОВОГО КАДРА (левый верхний угол картинки
    // — начало координат), а не окна редактора.
    bool m_mouseInside = false;
    float m_mouseX = -1.0f, m_mouseY = -1.0f;
    bool m_mouseDown = false;
    std::string m_typed;
};

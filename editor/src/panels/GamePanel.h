#pragma once

class EditorHost;

// Панель Game — «игровое окно»: изображение сцены от ИГРОВОЙ камеры (первая
// сущность с CameraComponent.Primary), а не от редакторской. Табом делит
// центр с Viewport; при входе в Play редактор вызывает RequestFocus() — таб
// Game выходит на передний план, как в привычных редакторах.
class GamePanel {
public:
    void Draw(EditorHost& host);

    // Запросить фокус панели (вызывается при StartPlay). Держим запрос
    // несколько кадров: одноразовый SetNextWindowFocus проигрывает фокусу
    // Viewport, который выставляет построение dock-раскладки в тот же кадр.
    void RequestFocus() { m_focusFrames = 3; }

private:
    int m_focusFrames = 0;
};

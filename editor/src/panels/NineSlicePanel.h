#pragma once
#include <memory>
#include <string>

#include "imgui.h"

#include "../EditorHost.h"
#include "sage/ui/NineSlice.h"

class Texture;

// ---------------------------------------------------------------------------
// РЕДАКТОР ДЕВЯТИНЫ — одно окно, в котором видно то, что правят.
//
// ЧТО БЫЛО НЕ ТАК. Окно правило СВОЮ копию нарезки, а на элемент она попадала
// только кнопкой «Применить к выбранному» — и только к части «Картинка». Числа
// в инспекторе оставались нулями, подложка кнопки и дорожка ползунка
// девятины не получали вовсе, а само окно при узкой ширине вылезало за свои
// края (и ImGui тысячами строк жаловался в лог на курсор, выведенный за
// границу окна).
//
// ЧТО ЗДЕСЬ.
//   • Окно открывается НА КОНКРЕТНОМ ВИДЕ конкретного элемента (картинка,
//     подложка, вид при наведении, дорожка...) и правит его ВЖИВУЮ: тянешь
//     линию — меняется элемент в сцене, каждое движение — одна запись в
//     истории отката. Без элемента — правится картинка и её .sage9.
//   • Слева — картинка (или её кусок из листа) крупно, поверх — четыре линии.
//     Углы, края и середина закрашены разными цветами, легенда рядом.
//   • Справа — числа (целые пиксели исходника), заполнение краёв и середины и
//     ПРЕДПРОСМОТР в размере самого элемента: ровно то, как он будет выглядеть.
//   • Всё раскладывается таблицей и дочерними окнами с прокруткой: ничего не
//     выходит за окно при любой его ширине, курсор никуда не переставляется.
// ---------------------------------------------------------------------------
class NineSlicePanel {
public:
    void Draw(EditorHost& host, bool& open);

    // Открыть окно на виде элемента или на картинке.
    void OpenFor(const EditorHost::NineSliceTarget& target);

    // Смена проекта: путь ассета в новом проекте ведёт к другому файлу.
    void ForgetProject();

private:
    // Что правится сейчас — прочитано из элемента (или из своей копии).
    struct Live {
        bool Bound = false;               // правим элемент
        std::string Path;                 // картинка
        glm::vec4 Sprite{0.0f};           // кусок листа (w <= 0 — весь файл)
        sage::ui::NineSlice Slice;
        float PixelScale = 1.0f;          // экранных пикселей на пиксель исходника
        glm::vec2 ElementSize{0.0f};      // размер элемента для предпросмотра
        std::string Title;                // «Кнопка › Подложка»
    };

    bool Read(EditorHost& host, Live& out);
    void Write(EditorHost& host, const sage::ui::NineSlice& slice);

    void DrawHeader(EditorHost& host, Live& live);
    void DrawCanvas(EditorHost& host, Live& live, ImVec2 size);
    void DrawSide(EditorHost& host, Live& live);
    void DrawPreview(const Live& live);

    void EnsureTexture(const std::string& path);

    EditorHost::NineSliceTarget m_target;
    // Без элемента нарезка живёт здесь (и в .sage9 рядом с картинкой).
    sage::ui::NineSlice m_fileSlice;
    std::string m_filePath;

    std::string m_texPath;
    std::shared_ptr<Texture> m_tex;

    float m_zoom = 0.0f;                  // 0 — вписать при следующем кадре
    ImVec2 m_pan{0.0f, 0.0f};
    int m_dragEdge = -1;                  // какую линию тащим (0..3), -1 — никакую
    bool m_showCheckers = true;

    glm::vec2 m_previewSize{0.0f};        // 0 — размер элемента
    std::string m_status;
};

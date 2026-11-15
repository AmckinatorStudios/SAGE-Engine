#pragma once
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "imgui.h"

#include "../EditorHost.h"
#include "SpriteEditModel.h"
#include "SpriteEditorBinding.h"

class Texture;

// ---------------------------------------------------------------------------
// РЕДАКТОР СПРАЙТОВ — нарезка картинки на именованные куски и их девятина.
//
// Окно — своё рабочее место: сверху строка документа (имя, файл, отмена,
// сохранение, несохранённое), под ней режимы «Sprite Sheet» и «Nine-Slice» на
// ОДНОМ холсте, ниже — свой док: холст в центре, инспектор справа (его можно
// оторвать, спрятать, вернуть раскладку по умолчанию; раскладка помнится).
//
// Нарезка — тремя способами (сетка, острова непрозрачных пикселей, руками),
// сетка и острова показываются на холсте ВЖИВУЮ, пока правят числа: кнопки
// «Предпросмотр» нет. «Create Sprites» заводит настоящие куски одним шагом
// истории. Острова ищутся в фоне: интерфейс не замирает, прежний результат
// виден, пока считается новый.
//
// Сохранение — «картинка.sagesheet» (куски, их точка опоры и девятина,
// настройки нарезки) и «картинка.sage9» (девятина картинки без кусков),
// атомарно. Данные и счёт — SpriteEditModel.h.
// ---------------------------------------------------------------------------
class SpriteEditor {
public:
    void Draw(EditorHost& host, bool* open);
    void ForgetProject();
    // Открыть картинку (и её лист) и выбрать кусок/последовательность.
    void Open(const std::string& path, int region = -1, int sequence = -1);
    // Режим «Nine-Slice» — для самопроверки и кнопки инспектора.
    void ShowNineSlice(bool on) { m_mode = on ? Mode::NineSlice : Mode::Sheet; }
    void ShowNineResult(bool on) { m_nineResult = on; }
    // Девятина из инспектора: вид элемента (свои значения элемента, правка
    // вживую через историю сцены) или картинка без элемента. Открывает
    // режим Nine-Slice на нужной текстуре и куске.
    void OpenFor(EditorHost& host, const EditorHost::NineSliceTarget& target);
    bool BoundToElement() const { return m_bound; }
    // «Create Sprites» без мыши — для снимка вопроса «Add / Replace».
    void CreateSpritesNow() { CreateSprites(); }

    enum class Mode { Sheet, NineSlice };
    enum class Tool { Select, Rectangle, Move, Resize, Pivot };

private:
    // --- Документ (SpriteEditor.cpp) -----------------------------------
    void Load(const std::string& path);
    void Snapshot();                       // перед правкой — в историю
    void Changed() { m_touched = true; }   // правка случилась: пересчитать «несохранено»
    void Undo();
    void Redo();
    bool Save();
    void DrawTopBar(EditorHost& host, bool* open);
    void DrawModeTabs();
    void DrawDock();
    void BuildDefaultLayout(ImGuiID dock);
    void DrawCloseModal(bool* open);
    void DrawReplaceModal();
    void HandleShortcuts();

    // --- Холст (SpriteEditor_Canvas.cpp) -------------------------------
    void DrawCanvasWindow(EditorHost& host);
    void DrawEmptyState(EditorHost& host);
    void DrawSheetCanvas(ImVec2 a, ImVec2 size);
    void DrawCanvasToolbar(ImVec2 a, ImVec2 size);
    void DrawStatusBar();
    void DrawSlicingPreview(ImDrawList* dl);
    void HandleSheetMouse(ImVec2 a, ImVec2 size);
    void UpdateView(ImVec2 a, ImVec2 size, float contentW, float contentH);
    ImVec2 ToScreen(glm::vec2 px) const;
    glm::vec2 ToPixel(ImVec2 screen) const;

    // --- Инспектор (SpriteEditor_Inspector.cpp) ------------------------
    void DrawInspectorWindow(EditorHost& host);
    void DrawSlicing(EditorHost& host);
    void DrawGridParams(EditorHost& host);
    void DrawAutoParams(EditorHost& host);
    void DrawSelected(EditorHost& host);
    void DrawPivot(EditorHost& host, std::vector<int>& sel);
    void DrawSequences(EditorHost& host);
    void DrawAtlas(EditorHost& host);
    void CreateSprites();
    void CommitSlices(const std::vector<glm::ivec4>& rects, const std::string& prefix,
                      sage::editor::sprite::SliceApply how);
    const std::vector<sage::ui::GridCell>& GridPreview();
    void PumpAuto();

    // --- Девятина (SpriteEditor_NineSlice.cpp) -------------------------
    void DrawNineCanvas(ImVec2 a, ImVec2 size);
    void DrawNineResult(ImVec2 a, ImVec2 size);
    void DrawNineInspector(EditorHost& host);
    // Что режется: выбранный кусок или вся картинка.
    glm::ivec4 NineSource() const;
    sage::ui::NineSlice NineGet() const;
    void NineSet(const sage::ui::NineSlice& n);

    bool Selected(int i) const;
    int Primary() const { return m_sel.empty() ? -1 : m_sel.back(); }
    void SelectOnly(int i);
    void DeleteSelected();
    void SetStatus(const std::string& text, bool error = false);

    // Картинка.
    std::string m_path;                    // ссылка ассета
    std::string m_disk;                    // настоящий путь на диске
    std::shared_ptr<Texture> m_tex;
    std::shared_ptr<const std::vector<unsigned char>> m_pixels;   // RGBA, строки сверху вниз
    int m_w = 0, m_h = 0;

    // Документ и история.
    sage::editor::sprite::SpriteDoc m_doc;
    sage::editor::sprite::SpriteHistory m_history;
    std::string m_savedPrint;
    bool m_dirty = false;
    bool m_touched = false;

    Mode m_mode = Mode::Sheet;
    Tool m_tool = Tool::Select;
    std::vector<int> m_sel;                // выбранные куски, последний — основной
    int m_hover = -1;

    // Вид холста: масштаб (экранных точек на пиксель) и сдвиг начала картинки
    // от середины холста.
    float m_zoom = 0.0f;                   // 0 — вписать при следующем кадре
    glm::vec2 m_pan{0.0f};
    ImVec2 m_canvasA{0, 0}, m_canvasSize{0, 0};
    float m_contentW = 0.0f, m_contentH = 0.0f;
    bool m_pixelGrid = true;
    bool m_userView = false;               // масштаб/сдвиг меняли руками — не вписывать заново
    glm::vec2 m_fitFor{0.0f};              // размер холста, под который вписано
    glm::vec2 m_cursorPx{-1.0f};

    // Перетаскивание по холсту.
    enum class Drag { None, Marquee, NewRect, Move, Resize, Pivot, Pan, Guide, PreviewSize };
    Drag m_drag = Drag::None;
    glm::vec2 m_dragFrom{0.0f};            // пиксель листа, где начали
    int m_dragHandle = -1;
    std::vector<glm::ivec4> m_dragOrig;    // прямоугольники выбранных до начала
    glm::ivec4 m_dragRect{0};              // рамка выделения / нового куска

    // Живой предпросмотр сетки — пересчитывается, только когда меняются числа.
    sage::ui::SpriteGrid m_gridKey;
    bool m_gridValid = false;
    std::vector<sage::ui::GridCell> m_gridCells;

    // Острова — в фоне, с паузой после правки числа (не считать на каждый
    // шаг перетаскивания).
    std::future<std::vector<glm::ivec4>> m_autoJob;
    sage::ui::SpriteAuto m_autoKey;        // с чем посчитан m_autoRects
    sage::ui::SpriteAuto m_autoWanted;     // с чем надо посчитать
    bool m_autoValid = false;
    bool m_autoRunning = false;
    double m_autoChangedAt = 0.0;
    std::vector<glm::ivec4> m_autoRects;

    // Вопросы.
    bool m_askClose = false;
    std::string m_pendingPath;             // открыть другую картинку после ответа на «несохранено»
    bool m_askReplace = false;
    std::vector<glm::ivec4> m_pendingRects;
    std::string m_pendingPrefix;

    // Раскладка.
    bool m_resetLayout = false;
    bool m_inspectorVisible = true;
    bool m_inspectorLoaded = false;

    // Поле имени выбранного куска.
    char m_nameBuf[128] = {};
    int m_nameFor = -1;
    bool m_nameBad = false;

    // Девятина вида элемента (OpenFor): пока привязка жива, линии и числа
    // правят элемент, а не лист и не .sage9.
    EditorHost* m_host = nullptr;
    bool m_bound = false;
    EditorHost::NineSliceTarget m_bind;
    sage::editor::sprite::BoundSlice m_boundSlice;
    bool BoundNine() const { return m_bound && m_mode == Mode::NineSlice; }
    void RefreshBound();

    // Девятина.
    int m_guide = -1;                      // 0..3 — какую линию тянут
    bool m_nineResult = false;             // показать результат, а не исходник
    glm::vec2 m_previewSize{192.0f, 96.0f};
    float m_nineZoom = 0.0f;

    // Последовательности.
    int m_seqSelected = -1;
    char m_seqName[64] = "run";
    char m_seqPrefix[64] = "sprite_";
    float m_previewTime = 0.0f;
    std::string m_atlasFolder;

    std::string m_status;
    bool m_statusError = false;
};

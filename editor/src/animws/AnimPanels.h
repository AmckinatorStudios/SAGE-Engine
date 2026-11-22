#pragma once
#include <string>

#include <glm/glm.hpp>

class EditorHost;
class AnimStage;

// ---------------------------------------------------------------------------
// ПАНЕЛИ ПРОСТРАНСТВА ANIMATION — всё про ОДНУ цель и её сцену предпросмотра
// (AnimStage), а не про уровень.
//
//   • Animation Hierarchy — кости скелета цели по их родительским связям
//     (как в файле модели, без своих папок), у цели без скелета — её объекты.
//   • Viewport — сцена предпросмотра: скелет поверх модели, выбор кости
//     щелчком, Move / Rotate / Scale (W / E / R), Local / World.
//   • Animation Inspector — цель, источник анимации, граф с параметрами,
//     слои и маски. Не Inspector сцены: тот правит объекты уровня, этот —
//     анимацию цели.
//
// Выбор кости — общий для всех трёх (AnimStage::SelectedBone): выбрал в
// иерархии — подсветилась во вьюпорте, и наоборот.
// ---------------------------------------------------------------------------

class AnimHierarchyPanel {
public:
    void Draw(EditorHost& host, bool* open, const std::string& windowId);
};

class AnimViewportPanel {
public:
    void Draw(EditorHost& host, bool* open, const std::string& windowId);
    // Отпустить захваченный курсор и жесты камеры: панель закрыли, спрятали
    // или ушли из пространства, пока кнопка мыши была зажата.
    void ReleaseCapture();
    // Состояние для самопроверки: что сейчас включено в тулбаре.
    bool ShowSkeleton() const { return m_showSkeleton; }
    int CurrentTool() const { return m_op; }
    bool CameraGestureActive() const { return m_flying || m_orbiting; }

    enum Tool { Move = 0, Rotate = 1, Scale = 2, SelectOnly = 3 };

private:
    void DrawToolbar(EditorHost& host, AnimStage& stage);
    void DrawBones(AnimStage& stage, const glm::mat4& viewProj, const glm::vec2& origin, const glm::vec2& size);
    void UpdateCamera(AnimStage& stage, bool hovered);
    int PickBone(AnimStage& stage, const glm::mat4& viewProj, const glm::vec2& origin, const glm::vec2& size,
                 const glm::vec2& mouse) const;
    void Manipulate(EditorHost& host, AnimStage& stage, const glm::vec2& origin, const glm::vec2& size);

    int m_op = Move;         // Tool
    bool m_local = true;     // Local / World
    bool m_showSkeleton = true;   // линии и точки костей поверх модели
    bool m_prefsLoaded = false;
    bool m_flying = false;   // ПКМ: осмотр и полёт
    bool m_orbiting = false; // средняя кнопка: орбита / с Shift — сдвиг
    bool m_captured = false; // курсор захвачен под жест
    float m_returnX = 0.0f, m_returnY = 0.0f;   // где был курсор до захвата
    bool m_gizmoWasUsing = false;
};

class AnimInspectorPanel {
public:
    void Draw(EditorHost& host, bool* open, const std::string& windowId);

private:
    void DrawTarget(EditorHost& host, AnimStage& stage);
    void DrawSource(EditorHost& host, AnimStage& stage);
    void DrawGraph(EditorHost& host, AnimStage& stage);
    void DrawLayers(EditorHost& host, AnimStage& stage);
    void DrawMasks(EditorHost& host, AnimStage& stage);

    char m_clipFilter[64] = {};   // поиск по списку клипов модели
    bool m_scrollToCurrent = true;
    int m_maskLayer = 0;    // слой, чья маска правится в разделе Animation Masks
    int m_lastSelection = -2;  // выбор в сцене, за которым следует цель
    bool m_graphFromPanel = false;  // граф предпросмотра ведёт окно графа
};

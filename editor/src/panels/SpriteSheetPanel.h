#pragma once
#include <memory>
#include <string>

#include "sage/ui/SpriteSheet.h"

class EditorHost;
class Texture;

// ---------------------------------------------------------------------------
// НАРЕЗКА СПРАЙТОВ — лист картинки на именованные куски (sage/ui/SpriteSheet.h).
//
// Картинка выбирается слотом. Нарезать — сеткой (размер клетки, промежутки,
// поле) или «Найти куски» — по непрозрачным островам. Куски видны рамками
// поверх картинки; щелчок выделяет кусок, справа — его имя, прямоугольник и
// девятина (если кусок — растягиваемая рамка). «Сохранить» пишет
// «картинка.sagesheet» рядом с картинкой; в игре кусок берут по имени
// (SetUISpriteNamed, sprite_sheet).
// ---------------------------------------------------------------------------
class SpriteSheetPanel {
public:
    void Draw(EditorHost& host, bool* open);
    void ForgetProject();
    // Открыть картинку (и её лист) и выбрать кусок/последовательность.
    void Open(const std::string& path, int region = -1, int sequence = -1);

private:
    void Load(const std::string& path);
    void DrawCanvas(float width, float height);
    void DrawRegion();
    void DrawSequences();
    void DrawAtlas(EditorHost& host);

    std::string m_path;
    std::string m_disk;                    // настоящий путь картинки на диске
    std::shared_ptr<Texture> m_tex;
    std::vector<unsigned char> m_pixels;   // RGBA, строки сверху вниз
    int m_w = 0, m_h = 0;
    sage::ui::SpriteSheet m_sheet;
    sage::ui::SpriteGrid m_grid;
    int m_selected = -1;
    float m_zoom = 0.0f;
    char m_nameBuf[128] = {};
    char m_prefixBuf[64] = "sprite";
    std::string m_status;
    bool m_dirty = false;
    // Рамка нового куска, которую тянут мышью по пустому месту (пиксели листа).
    bool m_dragging = false;
    glm::ivec2 m_dragFrom{0};
    glm::ivec2 m_dragTo{0};
    // Последовательности.
    int m_seqSelected = -1;
    char m_seqName[64] = "run";
    char m_seqPrefix[64] = "sprite_";
    float m_previewTime = 0.0f;
    // Атлас: папка картинок.
    std::string m_atlasFolder;
};

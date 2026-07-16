#pragma once
#include "UICanvas.h"
#include "Widgets.h"
#include "../asset/AssetManager.h"
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------
// UIManager — держит именованные UICanvas'ы и плоский реестр виджетов по
// целочисленному id, чтобы UI можно было полностью строить и обновлять из
// Lua (см. ScriptEngine — CreateCanvas/AddPanel/AddLabel/.../SetWidget*).
//
// До этого UICanvas собирался только руками в main.cpp (типизированные
// C++-указатели на виджеты). UIManager не заменяет этот способ — C++-код
// движка/игры по-прежнему может строить UICanvas напрямую, как раньше;
// UIManager — дополнительный слой НАД UICanvas специально для случая, когда
// виджет создаётся и настраивается из скрипта, где нет типизированных
// C++-указателей, а есть только простые значения (числа/строки), которые
// Lua может держать и передавать между вызовами.
//
// Часть ЯДРА движка — не знает ни про одну конкретную игру.
// ---------------------------------------------------------------------
class UIManager {
public:
    // Создаёт канвас с именем name, если его ещё нет; возвращает существующий,
    // если есть (идемпотентно, как и UICanvas этому идейно близка).
    UICanvas& CreateCanvas(const std::string& name) { return m_canvases[name]; }

    UICanvas* GetCanvas(const std::string& name) {
        auto it = m_canvases.find(name);
        return it == m_canvases.end() ? nullptr : &it->second;
    }

    // Регистрирует виджет (уже добавленный в какой-то UICanvas) в плоском
    // реестре и возвращает его id — то, чем Lua дальше ссылается на виджет
    // при SetWidget*. id 0 зарезервирован как "невалидный".
    int RegisterWidget(UIElement* widget) {
        m_widgets.push_back(widget);
        return static_cast<int>(m_widgets.size()); // 1-based: index+1
    }

    UIElement* GetWidget(int id) const {
        if (id <= 0 || id > static_cast<int>(m_widgets.size())) return nullptr;
        return m_widgets[static_cast<size_t>(id - 1)];
    }

    // Раз в кадр: прогоняет ввод по интерактивным виджетам ВСЕХ канвасов.
    void UpdateAll(const UIInputState& input, float screenW, float screenH) {
        for (auto& [name, canvas] : m_canvases) canvas.Update(input, screenW, screenH);
    }

    // Раз в кадр: рисует все канвасы (в порядке вставки в map — если важен
    // порядок отрисовки нескольких канвасов, держите их в одном канвасе
    // через несколько виджетов, либо создавайте канвасы в нужном порядке).
    void DrawAll(UIRenderer& ui) {
        for (auto& [name, canvas] : m_canvases) canvas.Draw(ui);
    }

    // Держит Asset<Texture> живым, пока им пользуется виджет-спрайт с данным
    // id — иначе AssetManager мог бы выгрузить текстуру, на которую виджет
    // хранит невладеющий указатель (см. UISprite::SpriteTexture).
    void KeepTextureAlive(int widgetId, Asset<Texture> texture) {
        m_spriteTextures[widgetId] = std::move(texture);
    }

    void Clear() {
        m_canvases.clear();
        m_widgets.clear();
        m_spriteTextures.clear();
    }

private:
    std::unordered_map<std::string, UICanvas> m_canvases;
    std::vector<UIElement*> m_widgets; // невладеющие — канвасы владеют через unique_ptr
    std::unordered_map<int, Asset<Texture>> m_spriteTextures;
};

#pragma once
#include <set>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "sage/scene/SceneReflect.h"
#include "ui/EditorPanel.h"

class EditorHost;

// ---------------------------------------------------------------------------
// Панель Inspector — свойства выбранного объекта (§11 ТЗ редактора).
//
// СЕКЦИИ И СТРОКИ ПОРОЖДАЮТСЯ ОПИСАНИЕМ, А НЕ КОДОМ. Панель не знает ни одного
// имени компонента: она спрашивает у сцены, что у объекта есть
// (sage::scene::ComponentRegistry), и по таблице свойств каждого компонента
// строит строки. Добавили поле в компонент — оно появилось здесь; удалили
// компонент — исчезла секция.
//
// Раньше это были две тысячи строк, где каждое поле каждого компонента
// расписано руками. Следствия были ровно те, которых и следовало ждать: поле,
// добавленное в движок, для человека не существовало, пока кто-то не вспомнит
// дописать его сюда; границы одного и того же угла в двух местах разъезжались.
//
// ПЕРЕСБОРКА — ПО СМЕНЕ ОБЪЕКТА, А НЕ КАЖДЫЙ КАДР. Строка держит указатель на
// сам компонент: он верен, пока выбран тот же объект, и обязан быть заменён,
// как только выбрали другой. Поэтому дерево панели пересобирается при смене
// выделения и при появлении/исчезновении компонента — и только тогда.
// ---------------------------------------------------------------------------
class InspectorPanelSage : public EditorPanel {
public:
    void SetHost(EditorHost* host) { m_host = host; }

    const char* PanelId() const override { return "inspector"; }
    const char* Title() const override { return "Inspector"; }
    void Build(sage::ui::sui::UIContext& ui, sage::ui::sui::UIElement* root) override;
    void Sync(float dt) override;
    // Ассет, брошенный на панель, назначается ВЫБРАННОМУ объекту: материал
    // красит его, скрипт вешается на него, модель заменяет его меш.
    void OnAssetDropped(const std::string& path, glm::vec2 at) override;
    bool AcceptsAssets() const override { return true; }

    // Сколько секций показано и какие — самопроверке: «инспектор показывает
    // ровно то, что есть у объекта» иначе проверяется только глазами.
    int SectionCount() const { return (int)m_sections.size(); }
    std::string SectionNames() const;

private:
    void Rebuild();
    void BuildComponent(const sage::scene::ComponentType& type);
    void BuildAddMenu();

    EditorHost* m_host = nullptr;
    sage::ui::sui::UIContext* m_ui = nullptr;

    sage::ui::sui::UIElement* m_header = nullptr;
    sage::ui::sui::TextInput* m_name = nullptr;
    sage::ui::sui::Label* m_id = nullptr;
    sage::ui::sui::PropertyGrid* m_grid = nullptr;
    sage::ui::sui::Label* m_empty = nullptr;
    sage::ui::sui::Button* m_add = nullptr;
    sage::ui::sui::Popup* m_addMenu = nullptr;

    std::vector<std::string> m_sections;
    // Слепок «кто выбран и что у него есть». Пересобирать инспектор на каждый
    // кадр значит терять каретку в поле и раскрытые секции при каждом движении
    // ползунка.
    std::size_t m_stamp = 0;
    int m_shownId = 0;
};

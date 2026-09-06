#pragma once

#include <set>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "ui/EditorPanel.h"

class EditorHost;
class Scene;

// ---------------------------------------------------------------------------
// Панель Hierarchy — ДЕРЕВО сущностей сцены (§6 ТЗ редактора).
//
// Выбор (в том числе множественный), раскрытие веток, поиск по имени, значок
// типа объекта, переключатель видимости, контекстное меню.
//
// НА SAGE UI. Панель не рисует себя каждый кадр: дерево собирается один раз, а
// дальше в него отдаётся ПЛОСКИЙ СПИСОК строк (Tree::SetItems), и виджет
// подгоняет под него свои строки. Из этого следует то, ради чего переход и
// затевался: прокрутка, раскрытые ветки и подсветка под курсором переживают
// смену выделения, а сцена из полутора сотен объектов не пересобирает полторы
// сотни узлов на каждый щелчок.
//
// СВОЕГО СОСТОЯНИЯ СЦЕНЫ ПАНЕЛЬ НЕ ДЕРЖИТ. Выделение — у EditorHost, иерархия
// и видимость — у сцены. Здесь живёт ровно то, что есть только у списка: какие
// ветки раскрыты и что набрано в поиске.
// ---------------------------------------------------------------------------
class HierarchyPanel : public EditorPanel {
public:
    // Хозяин ставится до первого Build: панель обращается к сцене и выделению.
    void SetHost(EditorHost* host) { m_host = host; }

    const char* PanelId() const override { return "hierarchy"; }
    const char* Title() const override { return "Hierarchy"; }
    void Build(sage::ui::sui::UIContext& ui, sage::ui::sui::UIElement* root) override;
    void Sync(float dt) override;
    // Ассет, брошенный НА СУЩНОСТЬ, относится к ней: материал красит её, скрипт
    // вешается на неё, модель заменяет её меш. Бросок в пустое место списка
    // означает другое — «добавить в сцену», — и различает их именно то, на что
    // попали.
    void OnAssetDropped(const std::string& path, glm::vec2 at) override;
    bool AcceptsAssets() const override { return true; }

    // Поиск снаружи: тем же путём, каким его ставит человек в поле. Нужен
    // самопроверке — иначе «поиск сужает список» проверяется только глазами.
    void SetFilter(const std::string& text);

    // Сколько строк видно прямо сейчас — самопроверке: «панель показывает ровно
    // то, что должна» иначе проверяется только глазами.
    int VisibleCount() const;
    // Имена видимых строк подряд — для той же самопроверки и для поиска.
    std::string VisibleText() const;

private:
    void Rebuild();                 // пересобрать плоский список из сцены
    void Collect(Scene& scene, entt::entity e, int depth,
                 std::vector<sage::ui::sui::TreeItem>& out);
    bool Matches(Scene& scene, entt::entity e) const;   // проходит ли поиск
    entt::entity Resolve(const std::string& rowId) const;

    EditorHost* m_host = nullptr;
    sage::ui::sui::UIContext* m_ui = nullptr;
    sage::ui::sui::Tree* m_tree = nullptr;
    sage::ui::sui::SearchBox* m_search = nullptr;
    sage::ui::sui::Label* m_summary = nullptr;
    sage::ui::sui::Popup* m_menu = nullptr;

    std::string m_filter;
    // Свёрнутые ветки, а не раскрытые: сцена открывается развёрнутой, и
    // хранить надо ИСКЛЮЧЕНИЯ — иначе каждый новый объект появлялся бы
    // свёрнутым, и его детей никто бы не увидел.
    std::set<int> m_collapsed;
    int m_menuTarget = 0;   // id объекта, по которому открыто контекстное меню

    // Слепок, по которому решается, надо ли пересобирать список. Дерево сцены
    // меняется куда реже, чем идёт кадр, а SetItems при равном списке всё равно
    // проходит по всем строкам.
    std::size_t m_stamp = 0;
};

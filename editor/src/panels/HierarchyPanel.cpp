#include "../PanelWindows.h"
#include "HierarchyPanel.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "AssetSlot.h"
#include "EditorIcons.h"
#include "Project.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/scene/Scene.h"
#include "../Localization.h"
#include "../ObjectCatalog.h"
#include "../FolderColors.h"

namespace {

// Какая иконка у сущности. Порядок проверок — от самого «говорящего»
// компонента к самому общему: у камеры со скриптом важнее, что это камера, а
// меш есть почти у всего и потому проверяется последним.
//
// ЗНАЧОК — ПРО ТИП ПРЕДМЕТА, А НЕ ПРО ЕГО ФОРМУ. Куб, сфера, цилиндр и конус
// получают ОДИН значок: в списке важно «это геометрия», а какая именно — видно
// в имени и во вьюпорте. Разные значки у форм означали бы, что глаз обязан
// различать кубик и шарик размером с букву, ничего за это не получая.
//
// СВЕТ — ЕДИНСТВЕННОЕ ИСКЛЮЧЕНИЕ, и не ради красоты: типы света ведут себя
// по-разному, и перепутать их дорого. Солнце одно на сцену и задаёт всё её
// настроение; точечный светит во все стороны; прожектор — конусом, и «свет не
// работает» у него обычно значит «смотрит не туда». Три значка отвечают на это
// без открывания инспектора.
const char* EntityIcon(entt::registry& reg, entt::entity e) {
    // Папка — прежде всего остального: она не предмет сцены, а ящик для них.
    if (reg.all_of<FolderComponent>(e)) {
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
        return (h && !h->Children.empty()) ? "folder-full" : "folder";
    }
    if (reg.all_of<CameraComponent>(e)) return "camera";
    if (const LightComponent* lc = reg.try_get<LightComponent>(e)) {
        switch (lc->Kind) {
            case LightComponent::Type::Directional: return "sun";
            // Прожектор — конус, и своего глифа-конуса в наборе нет. Капля
            // (drop) — ближайшее по форме: сужается книзу, то есть читается как
            // направленный пучок, а не как лампочка, светящая во все стороны.
            case LightComponent::Type::Spot: return "drop";
            default: return "light";
        }
    }
    if (reg.all_of<ReflectionProbeComponent>(e)) return "probe";
    if (reg.all_of<ParticleEmitterComponent>(e)) return "particles";
    if (reg.all_of<AnimationComponent>(e)) return "anim";
    if (reg.all_of<AudioSourceComponent>(e)) return "audio";
    if (reg.all_of<sage::ui::Transform>(e)) return "rect";
    if (reg.all_of<DecalComponent>(e)) return "texture";
    if (reg.all_of<RigidBodyComponent>(e) || reg.all_of<ColliderComponent>(e) ||
        reg.all_of<CharacterControllerComponent>(e))
        return "physics";
    if (reg.all_of<ScriptComponent>(e)) return "script";
    if (const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e)) {
        // Модель из файла — отдельно от встроенных форм: это ассет проекта, и
        // на него ссылаются, его переименовывают, он может не загрузиться.
        if (mr->Ref.type == MeshRef::Type::Model) return "model";
        if (mr->Ref.type != MeshRef::Type::None) return "cube"; // все формы — один значок
    }
    return "file";
}

// Цвет значка. У папки — её собственный (см. FolderComponent), у остальных —
// общий цвет значков редактора.
glm::vec3 EntityIconColor(entt::registry& reg, entt::entity e) {
    if (const FolderComponent* fc = reg.try_get<FolderComponent>(e)) return fc->Color;
    return glm::vec3(0.62f, 0.72f, 0.85f);
}

// Палитра меток — ОБЩАЯ с папками проекта (editor/src/FolderColors.h). Две
// палитры означали бы, что «зелёная папка» в дереве проекта и в списке сцены
// оказались разного зелёного, и цвет перестал бы быть общим языком.

} // namespace

const char* HierarchyPanel::IconFor(entt::registry& reg, entt::entity e) {
    return EntityIcon(reg, e);
}

// Рекурсивно рисует узел дерева: сам элемент (выбор/ПКМ/drag-drop) + детей.
void HierarchyPanel::DrawNode(EditorHost& host, Scene& scene, entt::entity e) {
    entt::registry& reg = scene.Registry();
    int id = reg.get<IdComponent>(e).Id;
    const std::string& name = reg.get<NameComponent>(e).Name;
    const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
    bool hasChildren = h && !h->Children.empty();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (host.IsSelected(id)) flags |= ImGuiTreeNodeFlags_Selected; // подсветка всех выбранных
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    ImGui::PushID(id);
    // Иконка рисуется ПОВЕРХ строки узла, а не отдельным элементом: узел ImGui
    // занимает всю ширину (SpanAvailWidth), и вставить перед ним что-либо
    // обычным способом нельзя — клик перестал бы попадать в строку.
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const float indent = ImGui::GetTreeNodeToLabelSpacing();
    bool open = ImGui::TreeNodeEx((void*)(intptr_t)id, flags, "  %s", name.c_str());

    // Иконка — Overlay, а НЕ Inline. Inline резервирует место под рисунок через
    // Dummy, то есть подаёт свой элемент, и «последним элементом» для ImGui
    // становится иконка. Всё, что спрашивает про последний элемент —
    // IsItemClicked, BeginDragDropSource, BeginDragDropTarget,
    // BeginPopupContextItem, — после этого отвечает про квадратик размером с
    // букву вместо строки дерева. Из-за этого в иерархии НЕ ВЫБИРАЛИСЬ объекты:
    // клик проверялся у иконки, попасть в которую можно было лишь случайно, а
    // заодно молча не работали перетаскивание и контекстное меню.
    {
        const float s = ImGui::GetTextLineHeight() * 0.86f;
        EditorIcons::Overlay(rowPos.x + indent - s * 1.15f,
                             rowPos.y + (ImGui::GetTextLineHeight() - s) * 0.5f, s,
                             EntityIcon(reg, e), EntityIconColor(reg, e));
    }

    // Рамка выделения: строка засчитывается, если её прямоугольник задет.
    // Проверка здесь, сразу после TreeNodeEx, — единственное место, где
    // прямоугольник ИМЕННО ЭТОЙ строки ещё «последний элемент» ImGui.
    if (m_rectActive && sage::editor::rectselect::Hits(m_rect, ImGui::GetItemRectMin(),
                                                      ImGui::GetItemRectMax())) {
        m_rectHits.push_back(id);
    }

    // Клик по строке (не по треугольнику раскрытия) — выбор. Ctrl — добавить/
    // убрать из набора (множественный выбор), обычный клик — одиночный.
    // Пока ведут рамку, клики не разбираем: жест уже начат, и «выбрать один»
    // посреди него означало бы мигание выбора.
    if (!m_rectActive && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        if (ImGui::GetIO().KeyCtrl) host.ToggleSelection(id);
        else host.SetSelectedId(id);
    }

    // Перетаскиваем эту сущность как источник.
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        ImGui::SetDragDropPayload("SAGE_ENTITY", &id, sizeof(int));
        ImGui::Text("%s", name.c_str());
        ImGui::EndDragDropSource();
    }
    // Бросили другую сущность на эту — делаем эту родителем.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ENTITY")) {
            int childId = *(const int*)p->Data;
            if (childId != id) {
                host.PushUndoSnapshot();
                scene.SetParentById(childId, id);
            }
        }
        // Ассет, брошенный НА СУЩНОСТЬ, относится к ней: материал красит её,
        // скрипт вешается на неё, модель заменяет её меш. Бросок в пустое место
        // списка (ниже) означает другое — «добавить в сцену», — и различает их
        // именно то, на что попали.
        // Ответ даётся ДО отпускания кнопки: подсказка говорит, что именно
        // произойдёт с этим файлом на этой сущности. Раньше бросок был
        // «наугад»: материал красил, модель заменяла меш, а .txt не делал
        // ничего — и все три случая выглядели одинаково, пока не отпустишь.
        const ImGuiDragDropFlags peek =
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH", peek)) {
            std::string dropped((const char*)p->Data, (size_t)p->DataSize);
            if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
            const char* what = nullptr;
            switch (assetslot::KindOf(dropped)) {
                case assetslot::Kind::Material: what = T("Assign the material to this object"); break;
                case assetslot::Kind::Model:    what = T("Replace this object's mesh"); break;
                case assetslot::Kind::Script:   what = T("Attach the script to this object"); break;
                case assetslot::Kind::Prefab:   what = T("Add the prefab as a child"); break;
                default: break;
            }
            ImGui::BeginTooltip();
            if (what) ImGui::TextUnformatted(what);
            else ImGui::TextDisabled("%s", T("This file cannot be applied to an object"));
            ImGui::EndTooltip();
            if (what) {
                const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRect(r0, r1, IM_COL32(120, 210, 130, 255), 3.0f, 0,
                                                    1.5f);
            }
            if (p->IsDelivery() && what) host.ApplyAssetToEntity(id, dropped);
        }
        ImGui::EndDragDropTarget();
    }

    // Контекстное меню сущности.
    if (ImGui::BeginPopupContextItem()) {
        // ПКМ по невыбранному — переключаемся на него; по выбранному в наборе —
        // сохраняем набор (Duplicate/Delete применятся ко всем выбранным).
        if (!host.IsSelected(id)) host.SetSelectedId(id);
        if (ImGui::MenuItem(T("Create Child"))) {
            host.PushUndoSnapshot();
            GameObject child = scene.CreateObject("Child");
            scene.SetParent(child.Entity(), e);
            host.SetSelectedId(child.Id());
        }
        if (ImGui::MenuItem(T("Create Folder Inside"))) {
            host.PushUndoSnapshot();
            GameObject folder = scene.CreateFolder(T("Folder"));
            scene.SetParent(folder.Entity(), e);
            host.SetSelectedId(folder.Id());
        }
        // ЦВЕТ — только у папки: у предмета сцены цвет уже занят материалом, и
        // вторая, ничего не значащая раскраска рядом путала бы.
        if (FolderComponent* fc = reg.try_get<FolderComponent>(e)) {
            if (ImGui::BeginMenu(T("Folder Colour"))) {
                for (const sage::editor::foldercolors::Tint& tint : sage::editor::foldercolors::Palette()) {
                    // Образец цвета рядом с названием: выбирают глазами, а
                    // список из восьми слов цвет не показывает.
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    const float box = ImGui::GetTextLineHeight();
                    const bool picked = ImGui::MenuItem((std::string("   ") + tint.Label).c_str());
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2(at.x + 2.0f, at.y + 2.0f), ImVec2(at.x + box - 2.0f, at.y + box - 2.0f),
                        ImGui::GetColorU32(ImVec4(tint.Color.r, tint.Color.g, tint.Color.b, 1.0f)),
                        3.0f);
                    if (picked) {
                        host.PushUndoSnapshot();
                        fc->Color = tint.Color;
                    }
                }
                ImGui::EndMenu();
            }
        }
        if (ImGui::MenuItem(T("Duplicate"))) host.DuplicateSelected();
        // Сохранить выбранную сущность (с детьми) как переиспользуемый префаб в
        // assets/ проекта. Имя файла — по имени сущности.
        if (ImGui::MenuItem(T("Save as Prefab"))) {
            std::error_code ec;
            std::filesystem::path dir = host.CurrentProject().Dir() / "assets";
            std::filesystem::create_directories(dir, ec);
            std::string safe = name;
            for (char& c : safe) if (c == '/' || c == '\\' || c == ':') c = '_';
            std::string perr;
            if (!host.SaveSelectedAsPrefab(dir / (safe + ".sageprefab"), perr))
                host.SetStatusMessage("Prefab save failed: " + perr);
        }
        bool hasParent = h && h->Parent != entt::null;
        if (ImGui::MenuItem(T("Unparent"), nullptr, false, hasParent)) {
            host.PushUndoSnapshot();
            scene.SetParent(e, entt::null);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T("Delete"))) host.DeleteSelected();
        ImGui::EndPopup();
    }

    if (open && hasChildren) {
        // Копия детей: SetParent во время обхода мог бы менять список.
        std::vector<entt::entity> kids = h->Children;
        std::sort(kids.begin(), kids.end(), [&](entt::entity a, entt::entity b) {
            return reg.get<IdComponent>(a).Id < reg.get<IdComponent>(b).Id;
        });
        for (auto k : kids)
            if (reg.valid(k)) DrawNode(host, scene, k);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void HierarchyPanel::Draw(EditorHost& host, bool* open) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();

    ImGui::Begin(T("Hierarchy" "###Hierarchy"), open, panelwindows::WindowFlags("Hierarchy"));
    // --- ДВЕ КНОПКИ НАД СПИСКОМ ---------------------------------------------
    //
    // Создать объект можно было двумя способами, и оба надо было ЗНАТЬ: меню
    // «Объект» наверху окна и правая кнопка по пустому месту списка. Ни то ни
    // другое не видно человеку, который смотрит на пустую сцену и ищет, с чего
    // начать. Кнопка прямо над списком отвечает на этот вопрос, не требуя
    // догадки; «Папка» рядом — потому что второе, что делают со списком после
    // наполнения, это наводят в нём порядок.
    if (EditorIcons::Button("plus", T("Object"), T("Add an object to the scene"))) {
        ImGui::OpenPopup("##hierarchy_add");
    }
    if (ImGui::BeginPopup("##hierarchy_add")) {
        if (const char* pick = sage::editor::objectcatalog::DrawMenu()) host.CreateCatalogObject(pick);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (EditorIcons::Button("folder-plus", T("Folder"),
                            T("A folder for sorting the list. It changes nothing in the game."))) {
        host.PushUndoSnapshot();
        // Папка создаётся ВНУТРИ выбранной папки, если выбрана именно папка:
        // раскладывая сцену, вложенные группы делают сразу, и лезть потом
        // перетаскивать только что созданную папку внутрь — лишний шаг.
        GameObject folder = scene.CreateFolder(T("Folder"));
        GameObject selected = scene.Get(host.SelectedId());
        if (selected.Valid() && scene.IsFolder(selected.Entity()))
            scene.SetParent(folder.Entity(), selected.Entity());
        host.SetSelectedId(folder.Id());
    }
    ImGui::SameLine();
    ImGui::TextDisabled(T("Scene: %s  |  Entities: %zu"), scene.Name().c_str(), scene.Count());
    ImGui::Separator();

    namespace rectselect = sage::editor::rectselect;
    m_rectActive = rectselect::Begin(m_rect);
    m_rectHits.clear();

    // Корни (без родителя) в стабильном порядке по id.
    std::vector<std::pair<int, entt::entity>> roots;
    auto view = reg.view<IdComponent, NameComponent>();
    for (auto e : view) {
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
        bool hasParent = h && h->Parent != entt::null && reg.valid(h->Parent);
        if (!hasParent) roots.push_back({view.get<IdComponent>(e).Id, e});
    }
    std::sort(roots.begin(), roots.end());
    for (auto& [id, e] : roots) DrawNode(host, scene, e);

    // Зона «в корень»: бросок сюда открепляет сущность от родителя, а
    // брошенный ассет добавляется в сцену как новый объект.
    ImGui::Dummy(ImVec2(-1.0f, 24.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ENTITY")) {
            int childId = *(const int*)p->Data;
            host.PushUndoSnapshot();
            scene.SetParentById(childId, -1); // -1 -> entt::null (в корень)
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH")) {
            std::string dropped((const char*)p->Data, (size_t)p->DataSize);
            if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
            // Точки под курсором тут нет — список это не трёхмерный вид,
            // — поэтому объект встаёт в начало координат, как при создании
            // через меню Entity.
            if (!host.AddAssetToScene(dropped))
                host.SetStatusMessage(T("Only a model or a prefab can be added to the scene"));
        }
        ImGui::EndDragDropTarget();
    }

    // Контекстное меню пустого места — создание корневых сущностей.
    //
    // ТОТ ЖЕ КАТАЛОГ, ЧТО И В МЕНЮ «ОБЪЕКТ». Здесь было два пункта («пустой» и
    // «куб») против полутора десятков в верхнем меню, и человек, нашедший
    // нужное там, здесь его не находил — при том что правой кнопкой по списку
    // объекты и создают.
    if (ImGui::BeginPopupContextWindow("##hierarchy_ctx",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (const char* pick = sage::editor::objectcatalog::DrawMenu()) host.CreateCatalogObject(pick);
        ImGui::EndPopup();
    }

    // Рамка выделения. Начинается в пустом месте списка — над строкой начинать
    // нельзя: там живут перетаскивание сущности и смена родителя.
    if (m_rectActive && m_rect.Finished && rectselect::Meaningful(m_rect)) {
        host.SetSelection(m_rectHits, m_rect.Additive);
    }
    rectselect::End(m_rect, ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                                !ImGui::IsAnyItemHovered() && !ImGui::IsPopupOpen("##hierarchy_ctx"));
    rectselect::Draw(m_rect);
    ImGui::End();
}

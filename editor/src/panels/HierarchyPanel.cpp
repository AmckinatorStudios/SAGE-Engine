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
#include "../ui/UI.h"
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
            // Прожектор — КОНУС: это буквально форма его светового пучка,
            // и объяснять её не нужно.
            case LightComponent::Type::Spot: return "cone";
            default: return "light";
        }
    }
    if (reg.all_of<ReflectionProbeComponent>(e)) return "probe";
    if (reg.all_of<ParticleEmitterComponent>(e)) return "particles";
    if (reg.all_of<AnimationComponent>(e)) return "anim";
    if (reg.all_of<AudioSourceComponent>(e)) return "audio";
    if (reg.all_of<sage::ui::Element>(e)) return "rect";
    if (reg.all_of<DecalComponent>(e)) return "texture";
    if (reg.all_of<RigidBodyComponent>(e) || reg.all_of<ColliderComponent>(e) ||
        reg.all_of<CharacterControllerComponent>(e))
        return "physics";
    if (reg.all_of<ScriptComponent>(e)) return "script";
    if (const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e)) {
        // Модель из файла — отдельно от встроенных форм: это ассет проекта, и
        // на него ссылаются, его переименовывают, он может не загрузиться.
        if (mr->Ref.type == MeshRef::Type::Model) return "model";
        // ВСЕ ВСТРОЕННЫЕ ФОРМЫ — ОДИН ЗНАЧОК. Своего значка у капсулы здесь
        // было исключение, и оно не оправдалось: в списке из полусотни строк
        // человек читает ИМЕНА, а значок отвечает на вопрос «объект какого
        // рода» — форма у него одна и та же, куб это или капсула.
        if (mr->Ref.type != MeshRef::Type::None) return "cube";
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

// ЛИНИИ ДЕРЕВА — СВОИ, а не встроенные в ImGui.
//
// ImGui ведёт горизонтальную чёрточку до СТРЕЛКИ раскрытия ребёнка, а значок и
// подпись у нас начинаются дальше — за отступом до подписи. Между чёрточкой и
// значком оставался провал в полтора десятка пикселей: линия будто обрывается
// на полпути, и дерево выглядит недорисованным. Здесь линия идёт до самого
// значка, а вертикаль — от родителя до СЕРЕДИНЫ последнего ребёнка, а не до
// конца всего поддерева: иначе она свисает под последней строкой в пустоту.
//
// Строки детей собираются в m_rows по ходу отрисовки — по ним и строится
// вертикаль: другого способа узнать, где кончился последний ребёнок, нет.
void HierarchyPanel::DrawTreeLines(const ImVec2& parentPos, float indent, int childDepth) {
    if (m_rows.empty()) return;
    const float line = ImGui::GetTextLineHeight();
    const float spineX = std::floor(parentPos.x + indent * 0.5f);
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_TreeLines);
    const float thickness = std::max(1.0f, ImGui::GetStyle().TreeLinesSize);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float lastMid = 0.0f;
    for (const Row& r : m_rows) {
        if (r.Depth != childDepth) continue;           // только прямые дети
        const float mid = std::floor(r.Y + line * 0.5f);
        lastMid = mid;
        // ЛИНИЯ НЕ ЛЕЗЕТ В СТРЕЛКУ. У строки с детьми в начале стоит стрелка
        // раскрытия, и горизонталь, доведённая до значка, шла ПРЯМО СКВОЗЬ неё
        // — стрелка выглядела перечёркнутой. У листа стрелки нет, и там линию
        // правильно вести до самого значка: иначе она обрывается в пустоте.
        const float end = r.Arrow ? r.ArrowX - 2.0f
                                  : r.IconX - EditorIcons::TextGap() * 0.5f;
        if (end <= spineX + 1.0f) continue;   // вести нечего: стрелка вплотную к вертикали
        dl->AddLine(ImVec2(spineX, mid), ImVec2(end, mid), col, thickness);
    }
    if (lastMid > 0.0f) {
        dl->AddLine(ImVec2(spineX, std::floor(parentPos.y + line)), ImVec2(spineX, lastMid), col,
                    thickness);
    }
    // Строки этого уровня уже использованы — дальше их не надо ни родителю, ни
    // соседям: иначе вертикаль тянулась бы через чужие ветки.
    m_rows.erase(std::remove_if(m_rows.begin(), m_rows.end(),
                                [childDepth](const Row& r) { return r.Depth >= childDepth; }),
                 m_rows.end());
}

// Рекурсивно рисует узел дерева: сам элемент (выбор/ПКМ/drag-drop) + детей.
void HierarchyPanel::DrawNode(EditorHost& host, Scene& scene, entt::entity e) {
    entt::registry& reg = scene.Registry();
    int id = reg.get<IdComponent>(e).Id;
    const std::string& name = reg.get<NameComponent>(e).Name;
    const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
    bool hasChildren = h && !h->Children.empty();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DrawLinesNone;
    if (host.Selection().Contains(id)) flags |= ImGuiTreeNodeFlags_Selected; // подсветка всех выбранных
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    ImGui::PushID(id);
    bool eyeClicked = false;
    // Иконка рисуется ПОВЕРХ строки узла, а не отдельным элементом: узел ImGui
    // занимает всю ширину (SpanAvailWidth), и вставить перед ним что-либо
    // обычным способом нельзя — клик перестал бы попадать в строку.
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const float indent = ImGui::GetTreeNodeToLabelSpacing();
    // УЗЕЛ БЕЗ ПОДПИСИ, подпись — рисунком вместе со значком.
    //
    // Подпись стояла в самом узле, а перед ней — два пробела, чтобы освободить
    // место под значок. Ширина пробела зависит от шрифта и его масштаба, и
    // «место под значок» то не хватало (значок наезжал на букву), то оказывалось
    // вдвое больше нужного. Значок и подпись — одна пара, и ставит их одна
    // функция с одним зазором на весь редактор (EditorIcons::DrawLabeled).
    bool open = ImGui::TreeNodeEx((void*)(intptr_t)id, flags, "%s", "");

    // Иконка — Overlay, а НЕ Inline. Inline резервирует место под рисунок через
    // Dummy, то есть подаёт свой элемент, и «последним элементом» для ImGui
    // становится иконка. Всё, что спрашивает про последний элемент —
    // IsItemClicked, BeginDragDropSource, BeginDragDropTarget,
    // BeginPopupContextItem, — после этого отвечает про квадратик размером с
    // букву вместо строки дерева. Из-за этого в иерархии НЕ ВЫБИРАЛИСЬ объекты:
    // клик проверялся у иконки, попасть в которую можно было лишь случайно, а
    // заодно молча не работали перетаскивание и контекстное меню.
    // Стрелка раскрытия ImGui рисует в начале строки, отступив FramePadding.x
    // (см. TreeNodeBehavior). Линия дерева обязана знать, где она: горизонталь,
    // доведённая до значка, проходила ПО стрелке насквозь.
    m_rows.push_back({rowPos.y, rowPos.x + indent,
                      rowPos.x + ImGui::GetStyle().FramePadding.x, hasChildren, m_depth});
    {
        const float line = ImGui::GetTextLineHeight();
        const glm::vec3 tint = EntityIconColor(reg, e);
        const ImVec4 c(tint.x, tint.y, tint.z, 1.0f);
        EditorIcons::DrawLabeled(ImGui::GetWindowDrawList(), ImVec2(rowPos.x + indent, rowPos.y),
                                 line, EntityIcon(reg, e), ImGui::GetColorU32(c), name.c_str(),
                                 ImGui::GetColorU32(ImGuiCol_Text));
    }

    // --- ВЫКЛЮЧАТЕЛЬ СПРАВА В СТРОКЕ ---------------------------------------
    //
    // Сцена собирается слоями, и на каждом шаге мешает всё остальное: сквозь
    // листву не видно геометрии, сквозь полсотни ламп не разобрать, что даёт
    // именно эта. Способ был один — удалить и сделать заново.
    //
    // Глаз стоит у ПРАВОГО КРАЯ, а не перед именем: перед именем он оттеснил бы
    // значок типа, по которому строку и находят, и щёлкать по нему пришлось бы,
    // целясь между стрелкой раскрытия и текстом. Справа он всегда на одном
    // месте, независимо от глубины вложения.
    {
        const bool hidden = reg.all_of<HiddenComponent>(e);
        // Выключенный РОДИТЕЛЬ гасит и эту строку — показываем это приглушённым
        // глазом: иначе объект не виден в кадре, а в списке выглядит включённым.
        const bool hiddenByParent = !hidden && scene.IsHidden(e);
        const float eyeSize = ImGui::GetTextLineHeight();
        const float right = ImGui::GetWindowContentRegionMax().x - eyeSize - 2.0f;
        const ImVec2 eyeAt(ImGui::GetWindowPos().x + right - ImGui::GetScrollX(),
                           rowPos.y + (ImGui::GetTextLineHeight() - eyeSize) * 0.5f);
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool overEye = mouse.x >= eyeAt.x && mouse.x <= eyeAt.x + eyeSize &&
                             mouse.y >= eyeAt.y && mouse.y <= eyeAt.y + eyeSize &&
                             ImGui::IsItemHovered();
        // Видимый объект — тусклый глаз, и только под курсором или у выключенного
        // он наливается цветом: полсотни ярких глаз подряд спорят с именами, по
        // которым список и читают.
        const glm::vec3 eyeColor = hidden ? glm::vec3(0.55f, 0.56f, 0.60f)
                                   : overEye ? glm::vec3(0.95f, 0.78f, 0.30f)
                                             : glm::vec3(0.42f, 0.44f, 0.50f);
        // ВЫКЛЮЧЕННЫЙ — ПЕРЕЧЁРКНУТЫЙ ГЛАЗ, а не замок. Замок означает «нельзя
        // трогать», и это другое свойство: объект может быть заперт и при этом
        // виден. Кнопка же переключает ровно видимость, и её выключенное
        // состояние обязано называться «не видно».
        EditorIcons::Overlay(eyeAt.x, eyeAt.y, eyeSize, hidden ? "eye-off" : "eye", eyeColor);
        if (overEye && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            host.PushUndoSnapshot();
            if (hidden) reg.remove<HiddenComponent>(e);
            else reg.emplace<HiddenComponent>(e);
            eyeClicked = true;
        }
        if (hiddenByParent) {
            // Приглушаем ВСЮ строку: так видно, что объект погашен не сам по
            // себе, а вместе с веткой, и щёлкать по его глазу бесполезно.
            ImGui::GetWindowDrawList()->AddRectFilled(
                rowPos, ImVec2(eyeAt.x, rowPos.y + ImGui::GetTextLineHeight()),
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.35f)));
        }
    }

    // Рамка выделения: строка засчитывается, если её прямоугольник задет.
    // Проверка здесь, сразу после TreeNodeEx, — единственное место, где
    // прямоугольник ИМЕННО ЭТОЙ строки ещё «последний элемент» ImGui.
    if (m_rectActive && sage::editor::rectselect::Hits(m_rect, ImGui::GetItemRectMin(),
                                                      ImGui::GetItemRectMax())) {
        m_rectHits.push_back(id);
    }
    m_visibleRows.push_back(id);   // порядок строк на экране — для Shift-диапазона
    {
        // Центр строки в экранных координатах: самопроверка щёлкает по списку
        // ПО-НАСТОЯЩЕМУ (см. EditorLayer::TickInputProbe), а чтобы щёлкнуть,
        // надо знать куда.
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        m_rowCenters.push_back(ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f));
    }

    // Клик по строке (не по треугольнику раскрытия) — выбор. Ctrl — добавить/
    // убрать из набора (множественный выбор), обычный клик — одиночный.
    // Пока ведут рамку, клики не разбираем: жест уже начат, и «выбрать один»
    // посреди него означало бы мигание выбора.
    if (!m_rectActive && !eyeClicked && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift && m_anchorId != -1) {
            m_shiftClickId = id;   // диапазон считается в конце кадра (см. HierarchyPanel.h)
        } else if (io.KeyCtrl) {
            host.Selection().Toggle(id);
            m_anchorId = id;
        } else {
            host.Selection().SetPrimary(id);
            m_anchorId = id;
        }
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
    // Отступы темы для меню: всплывающее окно наследует стиль, действующий в
    // момент открытия (см. Sage::UI::MenuScope). Блок держит время жизни
    // MenuScope в границах самого меню, а не всей отрисовки узла.
    if (Sage::UI::MenuScope rowMenu; ImGui::BeginPopupContextItem()) {
        // ПКМ по невыбранному — переключаемся на него; по выбранному в наборе —
        // сохраняем набор (Duplicate/Delete применятся ко всем выбранным).
        if (!host.Selection().Contains(id)) host.Selection().SetPrimary(id);
        if (EditorIcons::MenuItem("plus", T("Create Child"))) {
            host.PushUndoSnapshot();
            GameObject child = scene.CreateObject("Child");
            scene.SetParent(child.Entity(), e);
            host.Selection().SetPrimary(child.Id());
        }
        if (EditorIcons::MenuItem("folder-plus", T("Create Folder Inside"))) {
            host.PushUndoSnapshot();
            GameObject folder = scene.CreateFolder(T("Folder"));
            scene.SetParent(folder.Entity(), e);
            host.Selection().SetPrimary(folder.Id());
        }
        // ЦВЕТ — только у папки: у предмета сцены цвет уже занят материалом, и
        // вторая, ничего не значащая раскраска рядом путала бы.
        if (FolderComponent* fc = reg.try_get<FolderComponent>(e)) {
            if (EditorIcons::BeginMenu("color", T("Folder Colour"))) {
                for (const sage::editor::foldercolors::Tint& tint : sage::editor::foldercolors::Palette()) {
                    // Образец цвета рядом с названием: выбирают глазами, а
                    // список из восьми слов цвет не показывает.
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    const float box = ImGui::GetTextLineHeight();
                    // Место под образец — пробелами ровно по его ширине с общим
                    // зазором, а не «три пробела на глаз»: ширина пробела
                    // зависит от шрифта, и на другом масштабе подпись налезала
                    // на квадратик.
                    const float gap = EditorIcons::TextGap();
                    const float spaceW = ImGui::CalcTextSize(" ").x;
                    const int count = spaceW > 0.0f ? (int)std::ceil((box + gap) / spaceW) : 2;
                    const bool picked =
                        ImGui::MenuItem((std::string((size_t)count, ' ') + tint.Label).c_str());
                    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                    const float x = at.x + count * spaceW - gap - box;
                    const float y = std::floor(r0.y + ((r1.y - r0.y) - box) * 0.5f);
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2(x + 2.0f, y + 2.0f), ImVec2(x + box - 2.0f, y + box - 2.0f),
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
        if (EditorIcons::MenuItem("copy", T("Duplicate"), "Ctrl+D")) host.DuplicateSelected();
        // Сохранить выбранную сущность (с детьми) как переиспользуемый префаб в
        // assets/ проекта. Имя файла — по имени сущности.
        if (EditorIcons::MenuItem("prefab", T("Save as Prefab"))) {
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
        if (EditorIcons::MenuItem("up", T("Unparent"), nullptr, hasParent)) {
            host.PushUndoSnapshot();
            scene.SetParent(e, entt::null);
        }
        ImGui::Separator();
        if (EditorIcons::MenuItem("trash", T("Delete"), "Del")) host.DeleteSelected();
        ImGui::EndPopup();
    }

    if (open && hasChildren) {
        // Копия детей: SetParent во время обхода мог бы менять список.
        std::vector<entt::entity> kids = h->Children;
        std::sort(kids.begin(), kids.end(), [&](entt::entity a, entt::entity b) {
            return reg.get<IdComponent>(a).Id < reg.get<IdComponent>(b).Id;
        });
        const int myDepth = m_depth;
        ++m_depth;
        for (auto k : kids)
            if (reg.valid(k)) DrawNode(host, scene, k);
        --m_depth;
        DrawTreeLines(rowPos, indent, myDepth + 1);
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
    {
        // Отступы темы для меню: всплывающее окно наследует стиль, действующий в
        // момент открытия (см. Sage::UI::MenuScope). Блок держит время жизни
        // MenuScope в границах именно этого меню — иначе деструктор снимет
        // стиль только в конце Draw(), после End() текущего окна, и ImGui
        // решит, что стиль не сняли вовсе.
        Sage::UI::MenuScope addMenu;
        if (ImGui::BeginPopup("##hierarchy_add")) {
            if (const char* pick = sage::editor::objectcatalog::DrawMenu()) host.CreateCatalogObject(pick);
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();
    if (EditorIcons::Button("folder-plus", T("Folder"),
                            T("A folder for sorting the list. It changes nothing in the game."))) {
        host.PushUndoSnapshot();
        // Папка создаётся ВНУТРИ выбранной папки, если выбрана именно папка:
        // раскладывая сцену, вложенные группы делают сразу, и лезть потом
        // перетаскивать только что созданную папку внутрь — лишний шаг.
        GameObject folder = scene.CreateFolder(T("Folder"));
        GameObject selected = scene.Get(host.Selection().Primary());
        if (selected.Valid() && scene.IsFolder(selected.Entity()))
            scene.SetParent(folder.Entity(), selected.Entity());
        host.Selection().SetPrimary(folder.Id());
    }
    ImGui::Separator();

    namespace rectselect = sage::editor::rectselect;
    m_rectActive = rectselect::Begin(m_rect);
    m_rectHits.clear();
    m_visibleRows.clear();
    m_rowCenters.clear();

    // Корни (без родителя) в стабильном порядке по id.
    std::vector<std::pair<int, entt::entity>> roots;
    auto view = reg.view<IdComponent, NameComponent>();
    for (auto e : view) {
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
        bool hasParent = h && h->Parent != entt::null && reg.valid(h->Parent);
        if (!hasParent) roots.push_back({view.get<IdComponent>(e).Id, e});
    }
    std::sort(roots.begin(), roots.end());

    // --- САМА СЦЕНА — КОРЕНЬ СПИСКА, а не подпись над ним --------------------
    //
    // Имя сцены стояло строкой в заголовке панели, рядом с кнопками, и не
    // отвечало на вопрос, который задают списку: ГДЕ Я. Дерево начиналось сразу
    // с объектов, будто они висят в воздухе, а «в корень» было местом, которого
    // на экране нет, — пустотой под последней строкой. Сцена как узел ставит всё
    // на место: у дерева появляется вершина, у «корня» — строка, на которую
    // можно бросить объект, а у имени сцены — осмысленное место.
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    const ImVec2 rootPos = ImGui::GetCursorScreenPos();
    const bool sceneOpen = ImGui::TreeNodeEx(
        "##scene_root",
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_DrawLinesNone,
        "%s", "");
    {
        const ImVec4 tint(0.72f, 0.78f, 0.90f, 1.0f);
        EditorIcons::DrawLabeled(ImGui::GetWindowDrawList(),
                                 ImVec2(rootPos.x + ImGui::GetTreeNodeToLabelSpacing(), rootPos.y),
                                 ImGui::GetTextLineHeight(), "scene", ImGui::GetColorU32(tint),
                                 scene.Name().c_str(), ImGui::GetColorU32(ImGuiCol_Text));
    }
    // Бросок НА СЦЕНУ = «в корень»: то же, что и бросок в пустое место ниже, но
    // по видимой цели. Ассет, брошенный сюда, добавляется в сцену.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ENTITY")) {
            int childId = *(const int*)p->Data;
            host.PushUndoSnapshot();
            scene.SetParentById(childId, -1);
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH")) {
            std::string dropped((const char*)p->Data, (size_t)p->DataSize);
            if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
            if (!host.AddAssetToScene(dropped))
                host.SetStatusMessage(T("Only a model or a prefab can be added to the scene"));
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The scene itself: everything lives here"));

    if (sceneOpen) {
        m_rows.clear();
        m_depth = 0;
        for (auto& [id, e] : roots) DrawNode(host, scene, e);
        DrawTreeLines(rootPos, ImGui::GetTreeNodeToLabelSpacing(), 0);
        ImGui::TreePop();
    }

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
    // ДИАПАЗОН ОТ ЯКОРЯ ДО ЩЕЛЧКА — здесь, когда все строки кадра нарисованы.
    // Ctrl+Shift добавляет диапазон к набору, просто Shift — заменяет им набор.
    // Якорь не двигается: с него продолжают тянуть выбор дальше.
    if (m_shiftClickId != -1) {
        const auto from = std::find(m_visibleRows.begin(), m_visibleRows.end(), m_anchorId);
        const auto to = std::find(m_visibleRows.begin(), m_visibleRows.end(), m_shiftClickId);
        if (from != m_visibleRows.end() && to != m_visibleRows.end()) {
            auto a = from, b = to;
            if (a > b) std::swap(a, b);
            std::vector<int> range(a, b + 1);
            // Первичным (тем, что показывает инспектор) становится ТОТ, ПО ЧЕМУ
            // ЩЁЛКНУЛИ, а не последний по порядку: человек смотрит на него.
            range.erase(std::remove(range.begin(), range.end(), m_shiftClickId), range.end());
            range.push_back(m_shiftClickId);
            host.Selection().Set(range, ImGui::GetIO().KeyCtrl);
        } else {
            // Якорь свернули вместе с веткой — диапазона нет, но щелчок не
            // должен пропадать: выбираем то, по чему щёлкнули.
            host.Selection().SetPrimary(m_shiftClickId);
            m_anchorId = m_shiftClickId;
        }
        m_shiftClickId = -1;
    }

    if (m_rectActive && m_rect.Finished) {
        if (rectselect::Meaningful(m_rect)) {
            host.Selection().Set(m_rectHits, m_rect.Additive);
        } else if (!m_rect.Additive) {
            // ЩЕЛЧОК ПО ПУСТОМУ МЕСТУ СПИСКА СНИМАЕТ ВЫДЕЛЕНИЕ — так же, как во
            // вьюпорте. Раньше такой щелчок не делал ничего: рамка выходила
            // «незначащей», и выбор оставался висеть. Снять его можно было
            // только выбрав другой объект, то есть никак.
            host.Selection().SetPrimary(-1);
        }
    }
    rectselect::End(m_rect, ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                                !ImGui::IsAnyItemHovered() && !ImGui::IsPopupOpen("##hierarchy_ctx"));
    rectselect::Draw(m_rect);
    ImGui::End();
}

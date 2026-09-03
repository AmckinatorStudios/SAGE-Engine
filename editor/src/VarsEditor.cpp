#include "VarsEditor.h"

#include <cstring>
#include <vector>

#include <imgui.h>

#include "AssetSlot.h"
#include "EditorHost.h"
#include "EditorIcons.h"
#include "Localization.h"
#include "sage/scene/Scene.h"

namespace varsui {

namespace {

using sage::vars::Kind;
using sage::vars::Table;
using sage::vars::Value;
using sage::vars::Var;

// Названия видов для человека. Порядок — как в sage::vars::AllKinds().
const char* KindTitle(Kind k) {
    switch (k) {
        case Kind::Bool: return T("Yes / no");
        case Kind::Int: return T("Whole number");
        case Kind::Float: return T("Number");
        case Kind::String: return T("Text");
        case Kind::Vec2: return T("Two numbers");
        case Kind::Vec3: return T("Three numbers");
        case Kind::Color: return T("Colour");
        case Kind::Entity: return T("Object reference");
        case Kind::Asset: return T("File reference");
    }
    return "";
}

// Поле ввода строки фиксированной длины: ImGui правит буфер, а не std::string.
bool InputText(const char* id, std::string& text, const char* hint = nullptr) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", text.c_str());
    const bool changed = hint ? ImGui::InputTextWithHint(id, hint, buf, sizeof(buf))
                              : ImGui::InputText(id, buf, sizeof(buf));
    if (changed) text = buf;
    return changed;
}

} // namespace

bool DrawEntityRef(EditorHost& host, const char* id, sage::vars::EntityRef& ref) {
    Scene& scene = host.CurrentScene();
    GameObject pointed = ref.Valid() ? scene.Get(ref.Id) : GameObject{};
    // ЧТО ИМЕННО ПОКАЗЫВАТЬ. Имя, а не номер: номер сущности человек не видит
    // нигде и сверить его ему не с чем. Но и «ссылка есть, объекта нет» —
    // состояние, о котором надо сказать вслух: связь молча перестала работать.
    std::string label;
    if (!ref.Valid()) label = T("Nothing — drag an object here");
    else if (pointed.Valid()) label = pointed.Name();
    else label = std::string(T("Object is gone (id ")) + std::to_string(ref.Id) + ")";

    bool changed = false;
    ImGui::PushID(id);
    const bool missing = ref.Valid() && !pointed.Valid();
    if (missing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
    ImGui::Button(label.c_str(), ImVec2(-70.0f, 0.0f));
    if (missing) ImGui::PopStyleColor();

    // Приём броска из дерева иерархии — тем же типом нагрузки, каким дерево
    // начинает перетаскивание (HierarchyPanel).
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ENTITY")) {
            if (p->DataSize == (int)sizeof(int)) {
                ref.Id = *(const int*)p->Data;
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Drag an object from Hierarchy. The link holds the object's\n"
                                  "id, so renaming it does not break anything."));
    }
    // Двойной щелчок — «покажи мне его»: связь, ведущую неизвестно куда,
    // проверить иначе нечем.
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        pointed.Valid()) {
        host.SetSelectedId(ref.Id);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Clear##ref"))) {
        ref.Id = 0;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

bool DrawValue(EditorHost& host, const char* id, Value& value, const Var* meta,
               AssetPreview* preview) {
    ImGui::PushID(id);
    bool changed = false;
    const bool ranged = meta && meta->Min != meta->Max;

    switch (value.Type()) {
        case Kind::Bool: {
            bool v = value.AsBool();
            if (ImGui::Checkbox("##v", &v)) {
                // Галка меняется одним щелчком, «размазанной» правки у неё нет:
                // снимок берётся сразу, ДО присваивания.
                host.PushUndoSnapshot();
                value = Value(v);
                changed = true;
            }
            break;
        }
        case Kind::Int: {
            int v = value.AsInt();
            // Границы из объявления скрипта дают ПОЛЗУНОК: «здоровье от 0 до
            // 100» настраивается перетаскиванием, а не набором числа вслепую.
            const bool edited = ranged
                ? ImGui::SliderInt("##v", &v, (int)meta->Min, (int)meta->Max)
                : ImGui::DragInt("##v", &v);
            if (edited) { value = Value(v); changed = true; }
            break;
        }
        case Kind::Float: {
            float v = value.AsFloat();
            const bool edited = ranged ? ImGui::SliderFloat("##v", &v, meta->Min, meta->Max)
                                       : ImGui::DragFloat("##v", &v, 0.01f);
            if (edited) { value = Value(v); changed = true; }
            break;
        }
        case Kind::String: {
            std::string v = value.AsString();
            if (InputText("##v", v)) { value = Value(v); changed = true; }
            break;
        }
        case Kind::Vec2: {
            glm::vec2 v = value.AsVec2();
            if (ImGui::DragFloat2("##v", &v.x, 0.01f)) { value = Value(v); changed = true; }
            break;
        }
        case Kind::Vec3: {
            glm::vec3 v = value.AsVec3();
            if (ImGui::DragFloat3("##v", &v.x, 0.01f)) { value = Value(v); changed = true; }
            break;
        }
        case Kind::Color: {
            glm::vec4 v = value.AsVec4();
            if (ImGui::ColorEdit4("##v", &v.x, ImGuiColorEditFlags_NoInputs)) {
                value = Value(v);
                changed = true;
            }
            break;
        }
        case Kind::Entity: {
            sage::vars::EntityRef ref = value.AsEntity();
            if (DrawEntityRef(host, "ref", ref)) {
                host.PushUndoSnapshot();
                value = Value(ref);
                changed = true;
            }
            break;
        }
        case Kind::Asset: {
            // Тот же слот, что у меша и материала: обложка, приём броска с
            // проверкой типа, «показать в Assets». В ГРУППЕ, потому что слот
            // высокий и двигает курсор сам: без группы ячейка таблицы не знает
            // своей высоты, и ImGui жалуется на выход за границы окна.
            const std::string path = value.AsAsset().Path;
            ImGui::BeginGroup();
            const assetslot::Result r =
                assetslot::Draw(host, "asset", assetslot::Kind::Any, path, preview,
                                T("No file"));
            // Слот двигает курсор сам (обложка рисуется в своей позиции), а от
            // ЭЛЕМЕНТА в конце ImGui считает высоту ячейки. Без него окно не
            // знает, докуда выросло, и ругается на каждом кадре.
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::EndGroup();
            if (r.Changed) {
                host.PushUndoSnapshot();
                value = Value(sage::vars::AssetRef{r.Path});
                changed = true;
            }
            break;
        }
    }
    // ОТМЕНА У «РАЗМАЗАННЫХ» ПРАВОК. Перетаскивание ползунка и набор текста
    // меняют значение десятки раз за одно действие человека: снимок на каждое
    // изменение забил бы историю, а снимок ПОСЛЕ правки запомнил бы уже
    // изменённое состояние — то есть отмена не отменяла бы ничего. Track берёт
    // состояние на активации виджета и кладёт его в стек, когда правка
    // закончена; ровно так же работают все остальные поля инспектора.
    host.TrackLastImGuiItem();
    ImGui::PopID();
    return changed;
}

bool DrawTable(EditorHost& host, GameObject obj, Table& table, AssetPreview* preview) {
    bool changed = false;
    std::vector<Var>& vars = table.All();

    // ДВЕ КОЛОНКИ, а не строка из виджетов подряд. Разница видна сразу: имена
    // переменных разной длины, и без колонки значения встают лесенкой — читать
    // такой список глазами невозможно. Таблица ImGui ещё и переносит правый
    // столбец при узком инспекторе вместо того, чтобы обрезать его за краем.
    int removeAt = -1;
    if (ImGui::BeginTable("##vars", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);

        for (size_t i = 0; i < vars.size(); ++i) {
            Var& var = vars[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();

            // --- Имя -----------------------------------------------------------
            ImGui::TableSetColumnIndex(0);
            // Имя ОБЪЯВЛЕННОЙ переменной править нельзя: оно принадлежит коду, и
            // правка здесь означала бы, что скрипт молча перестал её находить.
            // Заведённую руками — можно: она ничья, кроме этого объекта.
            if (var.Declared) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(var.Title().c_str());
                if (ImGui::IsItemHovered()) {
                    // Настоящее имя показываем всегда: скрипт обращается к нему,
                    // а в списке стоит подпись, и связать одно с другим иначе
                    // нечем.
                    std::string tip = var.Name;
                    if (!var.Tooltip.empty()) tip += "\n" + var.Tooltip;
                    ImGui::SetTooltip("%s", tip.c_str());
                }
            } else {
                ImGui::SetNextItemWidth(-1.0f);
                if (InputText("##name", var.Name)) changed = true;
                host.TrackLastImGuiItem();
            }

            // --- Значение ------------------------------------------------------
            ImGui::TableSetColumnIndex(1);
            // Кнопка удаления и выбор вида — только у заведённых руками: у
            // объявленной и вид, и само существование задаёт скрипт, и кнопка
            // «убрать» обманывала бы (переменная вернётся при следующем чтении).
            // Ширина «во всю колонку минус хвост». Именно -1, а не -0, когда
            // хвоста нет: отрицательная ширина в ImGui значит «до правого края
            // минус столько», и -0 даёт нулевую ширину — поле просто исчезает.
            const float tail = var.Declared ? 1.0f : (ImGui::GetFrameHeight() * 2.4f +
                                                      ImGui::GetStyle().ItemSpacing.x * 2.0f);
            ImGui::SetNextItemWidth(-tail);
            if (DrawValue(host, "value", var.Data, &var, preview)) changed = true;

            if (!var.Declared) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 1.4f);
                int current = (int)var.Data.Type();
                std::vector<const char*> names;
                for (Kind k : sage::vars::AllKinds()) names.push_back(KindTitle(k));
                if (ImGui::Combo("##kind", &current, names.data(), (int)names.size())) {
                    // Смена вида СОХРАНЯЕТ введённое, насколько возможно:
                    // обнулять — терять набранное из-за одного щелчка.
                    var.Data = Value::Convert(var.Data, (Kind)current);
                    changed = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Kind of the value"));
                ImGui::SameLine();
                if (EditorIcons::Button("trash", "", T("Remove the variable"))) removeAt = (int)i;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (removeAt >= 0) {
        host.PushUndoSnapshot();
        vars.erase(vars.begin() + removeAt);
        changed = true;
    }

    if (ImGui::Button(T("Add variable"))) {
        host.PushUndoSnapshot();
        // Имя по умолчанию уникально: две «переменные» с одинаковым именем —
        // это одна переменная, и вторая молча пропала бы.
        std::string name = "value";
        for (int n = 1; table.Find(name); ++n) name = "value" + std::to_string(n);
        Var v;
        v.Name = name;
        v.Data = Value(0.0f);
        table.Put(v);
        changed = true;
    }
    if (vars.empty()) {
        ImGui::TextDisabled("%s", T("No public variables. A script declares them with a Vars\n"
                                    "table; here you can add your own."));
    }
    (void)obj;
    return changed;
}

bool DrawBindings(EditorHost& host, const char* id, sage::events::Bindings& bindings,
                  const std::vector<std::string>& triggers, AssetPreview* preview) {
    bool changed = false;
    ImGui::PushID(id);

    int removeAt = -1;
    for (size_t i = 0; i < bindings.size(); ++i) {
        sage::events::Binding& b = bindings[i];
        ImGui::PushID((int)i);

        // Заголовок связи читается как предложение: «при нажатии -> game.start».
        std::string title = b.Trigger.empty() ? T("always") : b.Trigger;
        title += "  ->  ";
        title += b.Event.empty() ? std::string(T("(no event)")) : b.Event;
        if (!b.Enabled) title += T("  (off)");

        const bool open = ImGui::TreeNodeEx("##b", ImGuiTreeNodeFlags_DefaultOpen, "%s",
                                            title.c_str());
        ImGui::SameLine();
        if (EditorIcons::Button("trash", "", T("Remove the link"))) removeAt = (int)i;
        if (open) {
            if (ImGui::Checkbox(T("Enabled##binding"), &b.Enabled)) {
                host.PushUndoSnapshot();
                changed = true;
            }

            // Когда.
            int trigger = 0;
            std::vector<const char*> names;
            for (size_t t = 0; t < triggers.size(); ++t) {
                names.push_back(triggers[t].c_str());
                if (triggers[t] == b.Trigger) trigger = (int)t;
            }
            if (!names.empty() && ImGui::Combo(T("When"), &trigger, names.data(), (int)names.size())) {
                host.PushUndoSnapshot();
                b.Trigger = triggers[(size_t)trigger];
                changed = true;
            }

            // Что: имя события. Свободной строкой, а не списком: имена событий
            // придумывает игра, и запирать их в список движка значило бы
            // ограничить ровно то, ради чего события и нужны.
            if (InputText(T("Event"), b.Event, T("for example: game.start"))) changed = true;
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s",
                                  T("Everyone subscribed to this name hears it: scripts and\n"
                                    "engine code alike."));
            }

            // Кому лично.
            ImGui::TextDisabled("%s", T("Call on object"));
            if (DrawEntityRef(host, "target", b.Target)) changed = true;
            if (b.Target.Valid()) {
                if (InputText(T("Method"), b.Method, "OnMessage")) changed = true;
                host.TrackLastImGuiItem();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s",
                                      T("The name reaches the object's script as OnMessage(entity,\n"
                                        "name, data) — the same path SendMessage uses."));
                }
            }

            // С чем. Вид и значение — РАЗНЫМИ строками: в одну они не влезают
            // ни при какой ширине инспектора (выбор вида широкий сам по себе),
            // и значение обрезалось бы ровно там, где его надо прочитать.
            int kind = (int)b.Arg.Type();
            std::vector<const char*> kindNames;
            for (sage::vars::Kind k : sage::vars::AllKinds()) kindNames.push_back(KindTitle(k));
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo(T("##argkind"), &kind, kindNames.data(), (int)kindNames.size())) {
                host.PushUndoSnapshot();
                b.Arg = Value::Convert(b.Arg, (sage::vars::Kind)kind);
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Kind of the argument"));
            ImGui::SetNextItemWidth(-1.0f);
            if (DrawValue(host, "arg", b.Arg, nullptr, preview)) changed = true;
            ImGui::TextDisabled("%s", T("Argument"));

            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (removeAt >= 0) {
        host.PushUndoSnapshot();
        bindings.erase(bindings.begin() + removeAt);
        changed = true;
    }
    if (ImGui::Button(T("Add link"))) {
        host.PushUndoSnapshot();
        sage::events::Binding b;
        b.Trigger = triggers.empty() ? std::string() : triggers.front();
        bindings.push_back(b);
        changed = true;
    }
    if (bindings.empty()) {
        ImGui::TextDisabled("%s", T("Nothing happens yet. A link lets this element send an event\n"
                                    "or call a method on another object by itself."));
    }
    ImGui::PopID();
    return changed;
}

} // namespace varsui

#pragma once
#include <cstddef>

#include "imgui.h"

#include "EditorHost.h"
#include "Localization.h"
#include "ui/UI.h"

// ---------------------------------------------------------------------------
// СБРОС К ЗНАЧЕНИЮ ПО УМОЛЧАНИЮ — одним жестом на любом поле.
//
// Подкрутил интенсивность света, потом ещё, потом забыл, какой она была, — и
// единственный способ вернуть «как было у нового» состоял в том, чтобы
// создать рядом новый объект и подсмотреть число. Правый щелчок по полю даёт
// меню «Сбросить к значению по умолчанию», а значок ↺ в заголовке
// компонента сбрасывает его целиком (см. EditorTheme::SectionHeader).
//
// ОТКУДА БЕРЁТСЯ «ПО УМОЛЧАНИЮ». Не из таблицы чисел, переписанной сюда
// второй раз (она разошлась бы с компонентом при первой же правке), а из
// самого компонента: ResetField смотрит то же поле у свежесозданного
// экземпляра (Owner{}) — ровно то значение, с которым объект появляется в
// сцене.
// ---------------------------------------------------------------------------
namespace sage::editor {

// Меню сброса у ПОСЛЕДНЕГО элемента (или группы — оберните составное поле
// в ImGui::BeginGroup/EndGroup). true — значение сброшено (снимок для отмены
// уже сделан).
template <class V>
bool ResetValue(EditorHost* host, V& value, const V& def) {
    bool changed = false;
    ImGui::PushID(static_cast<const void*>(&value));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem) &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##reset###ResetValue");
    if (Sage::UI::MenuScope menu; ImGui::BeginPopup("##reset###ResetValue")) {
        const bool same = value == def;
        ImGui::BeginDisabled(same);
        if (ImGui::MenuItem(T("Reset to default"))) {
            if (host) host->PushUndoSnapshot();
            value = def;
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// Значение поля field (лежащего внутри owner) у НОВОГО экземпляра Owner —
// то, с которым объект появляется в сцене. nullptr — поле не внутри owner.
template <class Owner, class Field>
const Field* DefaultFieldOf(const Owner& owner, const Field& field) {
    static const Owner kDefault{};
    const char* base = reinterpret_cast<const char*>(&owner);
    const char* at = reinterpret_cast<const char*>(&field);
    if (at < base || at + sizeof(Field) > base + sizeof(Owner)) return nullptr;
    return reinterpret_cast<const Field*>(reinterpret_cast<const char*>(&kDefault) + (at - base));
}

// Меню сброса со значением по умолчанию из нового экземпляра Owner.
template <class Owner, class Field>
bool ResetField(EditorHost* host, const Owner& owner, Field& field) {
    const Field* def = DefaultFieldOf(owner, field);
    return def ? ResetValue(host, field, *def) : false;
}

} // namespace sage::editor

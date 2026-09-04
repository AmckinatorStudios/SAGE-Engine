#include "TopBarPanel.h"
#include "EditorTheme.h"

#include <algorithm>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "../Localization.h"

namespace {

// Ширина, ниже которой панель начинает ужиматься. Числа не «на глаз»: при 1500
// в панель влезают все подписи, при 1150 — заголовки групп без подписей,
// дальше остаются одни иконки. Мерить точную ширину содержимого нечем — оно
// рисуется по ходу дела, — а измерять его вторым, невидимым проходом ради
// одной перестройки в год дороже, чем два порога.
constexpr float kWidthForLabels = 1500.0f;
constexpr float kWidthForTitles = 1150.0f;

struct Style {
    bool Labels = true;  // подписи рядом с иконками
    bool Titles = true;  // заголовки групп
};

// Заголовок группы: мелкая приглушённая строка над рядом кнопок. Когда
// заголовки выключены, вместо неё пустая строка той же высоты — иначе группы
// разъехались бы по вертикали относительно друг друга.
void GroupTitle(const Style& style, const char* title, const ImVec4* color = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Text,
                          color ? *color : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(style.Titles ? title : "");
    ImGui::PopStyleColor();
}

// Кнопка окна: подсвечена, когда окно открыто; щелчок переключает. Одним
// помощником, потому что кнопок десяток и разъехаться в поведении они не должны.
void PanelToggle(EditorHost& host, EditorPanel panel, const char* icon, const char* label,
                 const char* tip, bool withLabel) {
    bool& open = host.PanelVisible(panel);
    const bool pressed = withLabel ? EditorIcons::Button(icon, label, tip, open)
                                   : EditorIcons::IconOnlyButton(icon, tip, open);
    if (pressed) open = !open;
    ImGui::SameLine();
}

} // namespace

void TopBarPanel::Draw(EditorHost& host, float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 2));
    ImGui::BeginChild("##topbar", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    const float windowW = ImGui::GetWindowWidth();
    Style style;
    style.Labels = windowW >= kWidthForLabels;
    style.Titles = windowW >= kWidthForTitles;

    // Вертикальная черта между группами — от руки в списке отрисовки, а не
    // текстовым «|»: группа занимает две строки, и символ встал бы по базовой
    // линии первой из них, то есть посередине заголовка.
    const ImVec2 barMin = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto dividerAt = [&](float x) {
        x = std::floor(x);
        dl->AddLine(ImVec2(x, barMin.y + 5.0f), ImVec2(x, barMin.y + height - 5.0f),
                    IM_COL32(120, 128, 142, 130));
    };
    // Второй SameLine здесь НЕ РАБОТАЕТ: SameLine(0, spacing) считает позицию от
    // конца последнего ЭЛЕМЕНТА, а линия рисуется прямо в список отрисовки и
    // элементом не является. Два вызова подряд давали одну и ту же координату,
    // и черта оказывалась ровно под первой кнопкой следующей группы. Сдвиг —
    // руками.
    auto divider = [&]() {
        ImGui::SameLine(0.0f, 9.0f);
        const float x = ImGui::GetCursorPosX();
        dividerAt(barMin.x + x);
        ImGui::SetCursorPosX(x + 9.0f);
    };

    // --- Панели: из чего собрано рабочее место -------------------------------
    ImGui::BeginGroup();
    GroupTitle(style, T("Panels"));
    PanelToggle(host, EditorPanel::Hierarchy, "layout", T("Hierarchy"), T("Hierarchy"), false);
    PanelToggle(host, EditorPanel::Inspector, "file", T("Inspector"), T("Inspector"), false);
    PanelToggle(host, EditorPanel::Assets, "folder", T("Assets"), T("Assets"), false);
    PanelToggle(host, EditorPanel::Console, "debug", T("Console"), T("Console"), false);
    PanelToggle(host, EditorPanel::Code, "code", T("Code"), T("Code"), false);
    PanelToggle(host, EditorPanel::Profiler, "info", T("Profiler"), T("Profiler"), false);
    ImGui::NewLine();
    ImGui::EndGroup();

    divider();

    // --- Настройки: два окна и граница между ними ----------------------------
    //
    // Они стоят рядом намеренно. Вопрос «где настраивается свет» раньше не имел
    // ответа: часть была в окне Lighting, часть в Game Settings, часть на
    // объекте. Соседство подписанных кнопок — самая дешёвая форма ответа: среда
    // сцены здесь, цена кадра там, источники — объекты в иерархии.
    ImGui::BeginGroup();
    GroupTitle(style, T("Settings"));
    PanelToggle(host, EditorPanel::Environment, "sun", T("Environment"),
                T("Environment: sky, air, ambient light (saved with the scene)"), style.Labels);
    PanelToggle(host, EditorPanel::Settings, "gear", T("Game Settings"),
                T("Game Settings: quality and cost of the frame (saved with the project)"),
                style.Labels);
    PanelToggle(host, EditorPanel::UIEditor, "rect", T("Interface"),
                T("Interface editor: the game frame at its own resolution"), style.Labels);
    ImGui::NewLine();
    ImGui::EndGroup();

    divider();

    // --- Виды: чем смотреть на сцену ------------------------------------------
    ImGui::BeginGroup();
    GroupTitle(style, T("Views"));
    PanelToggle(host, EditorPanel::Viewport, "cube", T("Viewport"), T("Viewport"), style.Labels);
    PanelToggle(host, EditorPanel::Game, "camera", T("Game"),
                T("Game (view from the game camera)"), style.Labels);
    ImGui::NewLine();
    ImGui::EndGroup();

    // --- По центру: Play / Pause / Stop --------------------------------------
    //
    // Центрирование считается ОТ ОКНА и зажимается между левым и правым
    // блоками: ImGui::SameLine(x) с координатой левее курсора честно ставит
    // курсор назад, и блок рисуется ПОВЕРХ уже нарисованного — кнопки просто
    // исчезали с экрана.
    const float leftEnd = ImGui::GetItemRectMax().x - barMin.x;
    // Ширина слота под транспорт — ПОСТОЯННАЯ, хотя в правке в нём одна кнопка,
    // а в Play-режиме три. Иначе при входе в игру блок раздувался бы и кнопка
    // Play уезжала из-под курсора ровно в тот момент, когда по ней целятся
    // второй раз.
    const float playBlockW = 230.0f;
    const float rightBlockW = style.Titles ? 230.0f : 170.0f;
    const float spacing = 12.0f;
    // Середина СВОБОДНОГО МЕСТА, а не окна: подписи у кнопок сделали левый блок
    // широким, и «центр окна» оказывался под ним. ImGui::SameLine(x) с
    // координатой левее курсора честно ставит курсор назад, и транспорт
    // рисовался бы ПОВЕРХ кнопок — они просто исчезали с экрана.
    const float rightStart = windowW - rightBlockW;
    float playX = leftEnd + (rightStart - leftEnd - playBlockW) * 0.5f;
    const float playMax = rightStart - playBlockW - spacing;
    if (playX > playMax) playX = playMax;
    if (playX < leftEnd + spacing) playX = leftEnd + spacing;
    ImGui::SameLine(playX);
    dividerAt(barMin.x + playX - 9.0f);

    // Заголовок этой группы работает ещё и индикатором: в правке он просто
    // называет группу, а в Play-режиме цветом и словом говорит, что сцена
    // сейчас ЖИВАЯ. Отдельная надпись рядом с кнопками занимала бы место как
    // третья кнопка, а сказать ей нечего, пока идёт обычная правка.
    const EditorPlayState state = host.GetPlayState();
    const ImVec4 playing(0.40f, 0.90f, 0.40f, 1.0f);
    const ImVec4 paused(0.95f, 0.80f, 0.30f, 1.0f);
    ImGui::BeginGroup();
    switch (state) {
        case EditorPlayState::Playing: GroupTitle(style, T("PLAYING"), &playing); break;
        case EditorPlayState::Paused:  GroupTitle(style, T("PAUSED"), &paused); break;
        default:                       GroupTitle(style, T("Run")); break;
    }
    if (state == EditorPlayState::Editing) {
        // Цвет — по смыслу («утвердительное действие»), а не константой:
        // иначе в светлой теме кнопка осталась бы тёмно-зелёной на белом.
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::Ok));
        if (EditorIcons::Button("play", T("Play"), T("Run the scene (it is restored on Stop)")))
            host.StartPlay();
        ImGui::PopStyleColor();
    } else {
        if (state == EditorPlayState::Playing) {
            if (EditorIcons::Button("pause", T("Pause"), T("Pause"))) host.PausePlay();
        } else {
            if (EditorIcons::Button("play", T("Resume"), T("Resume"))) host.ResumePlay();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::Danger));
        if (EditorIcons::Button("stop", T("Stop"), T("Stop and restore the scene")))
            host.StopPlay();
        ImGui::PopStyleColor();
    }
    ImGui::NewLine();
    ImGui::EndGroup();

    // --- Справа: что открыто сейчас -------------------------------------------
    //
    // Имя сцены и пометка о несохранённых правках. В статус-баре они тоже есть,
    // но статус-бар внизу, а смотрят при работе — вверх, на кнопку Play.
    const float rightX = std::max(ImGui::GetItemRectMax().x - barMin.x + spacing,
                                  windowW - rightBlockW);
    ImGui::SameLine(rightX);
    dividerAt(barMin.x + rightX - 9.0f);
    ImGui::BeginGroup();
    GroupTitle(style, T("Scene"));
    ImGui::AlignTextToFramePadding();
    const bool dirty = host.SceneDirty();
    if (dirty) {
        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s *",
                           host.CurrentSceneName().c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("There are unsaved changes (Ctrl+S)"));
    } else {
        ImGui::TextDisabled("%s", host.CurrentSceneName().c_str());
    }
    ImGui::EndGroup();

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

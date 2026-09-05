#include "TopBarPanel.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "ui/UI.h"
#include "../Localization.h"

using sage::editor::T;
using EditorTheme::Role;

// ---------------------------------------------------------------------------
//  ВЕРХНЯЯ ПАНЕЛЬ — ОДНА СТРОКА.
//
//  ЧТО БЫЛО НЕ ТАК. Над каждой группой кнопок стояла мелкая подпись — «Панели»,
//  «Настройки», «Виды», «Запуск», «Сцена», — и панель из-за них была на самом
//  деле ДВУХСТРОЧНОЙ: строка подписей плюс строка кнопок. Высоты в 54 точки на
//  две строки не хватало, поэтому подписи прижимались к самому верху и
//  срезались краем панели, а кнопки вылезали снизу. Со стороны это читается
//  ровно так, как и было сказано: «кривое, кнопки за панель выходят».
//
//  Подписи при этом ничего не объясняли: что кнопка с солнцем открывает среду,
//  видно из подсказки при наведении, а не из слова «Настройки» над ней.
//
//  ЧТО СТАЛО. Одна строка, всё по вертикали по центру, группы разделены тонкой
//  чертой. Место, которое занимали подписи, отдано самим кнопкам — панель стала
//  НИЖЕ и при этом просторнее внутри.
//
//  Все размеры — из токенов (ui/UIStyle.h). Раньше здесь стояли 7, 9, 12, 230,
//  170 и цвет черты числом; из-за них панель и не совпадала ни с чем вокруг.
// ---------------------------------------------------------------------------
namespace {

// Пороги ужимания. Сначала пропадают подписи у кнопок, потом имя сцены справа.
// Мерить точную ширину содержимого нечем — оно рисуется по ходу дела.
constexpr float kWidthForLabels = 1400.0f;
constexpr float kWidthForScene = 900.0f;

struct Row {
    float Height = 0.0f;   // высота панели
    float Top = 0.0f;      // Y кнопки, чтобы строка стояла по центру
};

// Поставить курсор так, чтобы элемент высотой ControlHeight встал по центру
// панели. Без этого ряд «плавает»: у кнопки с подписью и у кнопки-иконки
// разная высота, и они выравниваются по верху.
void CenterY(const Row& row) { ImGui::SetCursorPosY(row.Top); }

// Кнопка окна: подсвечена, когда окно открыто; щелчок переключает. Одним
// помощником, потому что кнопок десяток и разъехаться в поведении они не должны.
void PanelToggle(EditorHost& host, EditorPanel panel, const char* icon, const char* label,
                 const char* tip, bool withLabel, const Row& row) {
    bool& open = host.PanelVisible(panel);
    CenterY(row);
    const bool pressed = withLabel ? EditorIcons::Button(icon, label, tip, open)
                                   : EditorIcons::IconOnlyButton(icon, tip, open);
    if (pressed) open = !open;
    ImGui::SameLine(0.0f, Sage::UI::Get().SpacingXS);
}

} // namespace

void TopBarPanel::Draw(EditorHost& host, float height) {
    const Sage::UI::Style& ui = Sage::UI::Get();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui.PaddingControl, ui.PaddingControlY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui.SpacingXS, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(Role::Bg));
    ImGui::BeginChild("##topbar", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    const float windowW = ImGui::GetWindowWidth();
    const bool labels = windowW >= kWidthForLabels;
    const bool showScene = windowW >= kWidthForScene;

    Row row;
    row.Height = height;
    row.Top = std::floor((height - ui.ControlHeight) * 0.5f);

    const ImVec2 barMin = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Черта между группами — рисунком, а не текстовым «|»: символ встал бы по
    // базовой линии текста, то есть не по центру панели.
    auto dividerAt = [&](float x) {
        x = std::floor(x);
        const float inset = ui.SpacingSM;
        dl->AddLine(ImVec2(x, barMin.y + inset), ImVec2(x, barMin.y + height - inset),
                    ImGui::GetColorU32(EditorTheme::Color(Role::Line)), ui.BorderWidth);
    };
    // Второй SameLine здесь НЕ РАБОТАЕТ: SameLine(0, spacing) считает позицию от
    // конца последнего ЭЛЕМЕНТА, а линия элементом не является. Сдвиг — руками.
    auto divider = [&]() {
        ImGui::SameLine(0.0f, ui.SpacingMD);
        const float x = ImGui::GetCursorPosX();
        dividerAt(barMin.x + x);
        ImGui::SetCursorPosX(x + ui.SpacingMD);
    };

    ImGui::SetCursorPosX(ui.PaddingPanel);

    // --- Панели: из чего собрано рабочее место -------------------------------
    PanelToggle(host, EditorPanel::Hierarchy, "layout", T("Hierarchy"), T("Hierarchy"), false, row);
    PanelToggle(host, EditorPanel::Inspector, "file", T("Inspector"), T("Inspector"), false, row);
    PanelToggle(host, EditorPanel::Assets, "folder", T("Assets"), T("Assets"), false, row);
    PanelToggle(host, EditorPanel::Console, "debug", T("Console"), T("Console"), false, row);
    PanelToggle(host, EditorPanel::Code, "code", T("Code"), T("Code"), false, row);
    PanelToggle(host, EditorPanel::Profiler, "info", T("Profiler"), T("Profiler"), false, row);

    divider();

    // --- Настройки: два окна и граница между ними ----------------------------
    //
    // Они стоят рядом намеренно. Вопрос «где настраивается свет» раньше не имел
    // ответа: часть была в окне Lighting, часть в Game Settings, часть на
    // объекте. Соседство кнопок — самая дешёвая форма ответа: среда сцены
    // здесь, цена кадра там, источники — объекты в иерархии.
    PanelToggle(host, EditorPanel::Environment, "sun", T("Environment"),
                T("Environment: sky, air, ambient light (saved with the scene)"), labels, row);
    PanelToggle(host, EditorPanel::Settings, "gear", T("Game Settings"),
                T("Game Settings: quality and cost of the frame (saved with the project)"),
                labels, row);
    PanelToggle(host, EditorPanel::UIEditor, "rect", T("Interface"),
                T("Interface editor: the game frame at its own resolution"), labels, row);

    divider();

    // --- Виды: чем смотреть на сцену ------------------------------------------
    PanelToggle(host, EditorPanel::Viewport, "cube", T("Viewport"), T("Viewport"), labels, row);
    PanelToggle(host, EditorPanel::Game, "camera", T("Game"),
                T("Game (view from the game camera)"), labels, row);

    // --- По центру: Play / Pause / Stop --------------------------------------
    //
    // Центрирование считается ОТ СВОБОДНОГО МЕСТА и зажимается между левым и
    // правым блоками: ImGui::SameLine(x) с координатой левее курсора честно
    // ставит курсор назад, и блок рисуется ПОВЕРХ уже нарисованного — кнопки
    // просто исчезали бы с экрана.
    const float leftEnd = ImGui::GetItemRectMax().x - barMin.x;
    // Ширина слота под транспорт ПОСТОЯННА, хотя в правке в нём одна кнопка, а
    // в игре две. Иначе при входе в игру блок раздувался бы и кнопка уезжала
    // из-под курсора ровно в тот момент, когда по ней целятся второй раз.
    const float playBlockW = ui.ControlHeight * 7.0f;
    const float sceneBlockW = showScene ? ui.ControlHeight * 8.0f : 0.0f;
    const float gap = ui.SpacingMD;

    const float rightStart = windowW - sceneBlockW - ui.PaddingPanel;
    float playX = leftEnd + (rightStart - leftEnd - playBlockW) * 0.5f;
    playX = std::min(playX, rightStart - playBlockW - gap);
    playX = std::max(playX, leftEnd + gap);
    ImGui::SameLine(playX);
    dividerAt(barMin.x + playX - gap);

    const EditorPlayState state = host.GetPlayState();
    CenterY(row);
    if (state == EditorPlayState::Editing) {
        // ГЛАВНОЕ ДЕЙСТВИЕ ЭКРАНА — единственное, что красится акцентом.
        // Именно ради него жёлтый и держат в резерве: когда им покрашено ещё
        // пять кнопок, эта перестаёт быть заметной.
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(Role::Accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Color(Role::AccentHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Color(Role::AccentActive));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextOnAccent));
        if (EditorIcons::Button("play", T("Play"), T("Run the scene (it is restored on Stop)")))
            host.StartPlay();
        ImGui::PopStyleColor(4);
    } else {
        if (state == EditorPlayState::Playing) {
            if (EditorIcons::Button("pause", T("Pause"), T("Pause"))) host.PausePlay();
        } else {
            if (EditorIcons::Button("play", T("Resume"), T("Resume"))) host.ResumePlay();
        }
        ImGui::SameLine(0.0f, ui.SpacingXS);
        CenterY(row);
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(Role::Danger));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextOnAccent));
        if (EditorIcons::Button("stop", T("Stop"), T("Stop and restore the scene")))
            host.StopPlay();
        ImGui::PopStyleColor(2);

        // Состояние игры — значком рядом с кнопками, а не подписью над ними:
        // подпись стоила целой строки высоты, а сказать ей нечего, пока идёт
        // обычная правка.
        ImGui::SameLine(0.0f, ui.SpacingSM);
        CenterY(row);
        ImGui::AlignTextToFramePadding();
        const bool playing = state == EditorPlayState::Playing;
        ImGui::TextColored(EditorTheme::Color(playing ? Role::Ok : Role::Warn), "%s",
                           playing ? T("PLAYING") : T("PAUSED"));
    }

    // --- Справа: что открыто сейчас -------------------------------------------
    //
    // Имя сцены и пометка о несохранённых правках. В статус-баре они тоже есть,
    // но статус-бар внизу, а смотрят при работе — вверх, на кнопку Play.
    if (showScene) {
        const float rightX = std::max(ImGui::GetItemRectMax().x - barMin.x + gap, rightStart);
        ImGui::SameLine(rightX);
        dividerAt(barMin.x + rightX - gap);
        CenterY(row);
        ImGui::AlignTextToFramePadding();
        EditorIcons::Inline("scene");
        ImGui::SameLine(0.0f, ui.SpacingXS);
        // Длинное имя сцены УКОРАЧИВАЕТСЯ, а не выталкивает себя за край панели.
        const float room = windowW - ImGui::GetCursorPosX() - ui.PaddingPanel;
        const std::string name = host.CurrentSceneName() + (host.SceneDirty() ? " *" : "");
        const std::string shown = Sage::UI::Truncate(name.c_str(), room);
        if (host.SceneDirty()) {
            ImGui::TextColored(EditorTheme::Color(Role::Warn), "%s", shown.c_str());
            Sage::UI::Tooltip(T("There are unsaved changes (Ctrl+S)"), "Ctrl+S");
        } else {
            ImGui::TextDisabled("%s", shown.c_str());
            if (shown != name) Sage::UI::Tooltip(name.c_str());
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

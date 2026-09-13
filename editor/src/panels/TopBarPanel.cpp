#include "TopBarPanel.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"

#include "EditorHost.h"
#include "Project.h"
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

    // --- СЛЕВА: ФАЙЛ -------------------------------------------------------
    //
    // Четыре действия, которыми начинают и заканчивают работу: новая сцена,
    // открыть сцену, открыть проект, сохранить. Раньше верхнюю панель занимали
    // переключатели окон — но окна открывают раз в день, а сохраняют раз в
    // минуту, и место у левого края (куда рука идёт первой) досталось не тому.
    // Сами окна никуда не делись: они все перечислены в меню «Окно».
    CenterY(row);
    if (EditorIcons::IconOnlyButton("file", T("New scene"))) host.RequestDialog("New Scene");
    ImGui::SameLine(0.0f, ui.SpacingXS);
    CenterY(row);
    if (EditorIcons::IconOnlyButton("open", T("Open scene..."))) host.RequestDialog("Open Scene");
    ImGui::SameLine(0.0f, ui.SpacingXS);
    CenterY(row);
    if (EditorIcons::IconOnlyButton("project", T("Open project..."))) host.RequestDialog("Open Project");
    ImGui::SameLine(0.0f, ui.SpacingXS);
    CenterY(row);
    if (EditorIcons::IconOnlyButton("save", T("Save scene (Ctrl+S)"))) host.RequestDialog("Save Scene");

    divider();

    // --- ОТМЕНА И ПОВТОР ---------------------------------------------------
    //
    // Гаснут, когда отменять нечего: серая кнопка честно говорит «здесь пусто»,
    // а живая, которая ничего не делает, читается как поломка.
    const bool canUndo = host.CanUndo() && !host.InPlayMode();
    const bool canRedo = host.CanRedo() && !host.InPlayMode();
    CenterY(row);
    ImGui::BeginDisabled(!canUndo);
    if (EditorIcons::IconOnlyButton("undo", T("Undo (Ctrl+Z)"))) host.Undo();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, ui.SpacingXS);
    CenterY(row);
    ImGui::BeginDisabled(!canRedo);
    if (EditorIcons::IconOnlyButton("redo", T("Redo (Ctrl+Y)"))) host.Redo();
    ImGui::EndDisabled();

    // --- ПО ЦЕНТРУ: ЗАПУСК --------------------------------------------------
    //
    // Центрирование считается ОТ СВОБОДНОГО МЕСТА и зажимается между левым и
    // правым блоками: ImGui::SameLine(x) с координатой левее курсора честно
    // ставит курсор назад, и блок рисуется ПОВЕРХ уже нарисованного — кнопки
    // просто исчезали бы с экрана.
    const float leftEnd = ImGui::GetItemRectMax().x - barMin.x;
    // Ширина слота под запуск ПОСТОЯННА, хотя в правке в нём одна кнопка, а в
    // игре четыре. Иначе при входе в игру блок раздувался бы и кнопка уезжала
    // из-под курсора ровно в тот момент, когда по ней целятся второй раз.
    const float playBlockW = ui.ControlHeight * 8.0f;
    const float rightBlockW = ui.ControlHeight * 3.0f;
    const float gap = ui.SpacingMD;

    const float rightStart = windowW - rightBlockW - ui.PaddingPanel;
    float playX = leftEnd + (rightStart - leftEnd - playBlockW) * 0.5f;
    playX = std::min(playX, rightStart - playBlockW - gap);
    playX = std::max(playX, leftEnd + gap);
    ImGui::SameLine(playX);

    const EditorPlayState state = host.GetPlayState();
    const bool playing = state == EditorPlayState::Playing;
    const bool paused = state == EditorPlayState::Paused;
    CenterY(row);
    // ГЛАВНОЕ ДЕЙСТВИЕ ЭКРАНА — единственное, что красится акцентом. Именно
    // ради него жёлтый и держат в резерве: когда им покрашено ещё пять кнопок,
    // эта перестаёт быть заметной. Подпись у него есть, у остальных нет: «Play»
    // ищут глазами, а паузу и стоп — уже рядом с ним.
    ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(Role::Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Color(Role::AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Color(Role::AccentActive));
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextOnAccent));
    if (paused) {
        if (EditorIcons::Button("play", T("Resume"), T("Resume"))) host.ResumePlay();
    } else if (EditorIcons::Button("play", T("Play"), T("Run the scene (it is restored on Stop)"),
                                   playing)) {
        if (!playing) host.StartPlay();
    }
    ImGui::PopStyleColor(4);

    // Пауза, шаг и стоп — только когда есть что останавливать. В правке их
    // место остаётся пустым, а не занято серыми кнопками: пустое место не
    // предлагает нажать на себя.
    ImGui::SameLine(0.0f, ui.SpacingXS);
    CenterY(row);
    ImGui::BeginDisabled(!playing);
    if (EditorIcons::IconOnlyButton("pause", T("Pause"))) host.PausePlay();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, ui.SpacingXS);
    CenterY(row);
    // ШАГ — только на паузе: у работающей игры он смысла не имеет.
    ImGui::BeginDisabled(!paused);
    if (EditorIcons::IconOnlyButton("step", T("One frame forward (only while paused)")))
        host.StepPlay();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, ui.SpacingXS);
    CenterY(row);
    ImGui::BeginDisabled(!host.InPlayMode());
    if (EditorIcons::IconOnlyButton("stop", T("Stop and restore the scene"))) host.StopPlay();
    ImGui::EndDisabled();

    // --- ПРИВЯЗКА -----------------------------------------------------------
    //
    // Здесь, а не только во вьюпорте: это свойство ЖЕСТА, а не окна, и человек
    // читает его тем же взглядом, каким смотрит на «Play».
    //
    // А ВОТ ПРОСТРАНСТВА ОСЕЙ ЗДЕСЬ БОЛЬШЕ НЕТ. Тот же переключатель стоит в
    // строке инструментов вьюпорта — у самого объекта, которого он касается, —
    // и две кнопки для одного состояния читаются как две РАЗНЫЕ настройки:
    // человек щёлкает одну, видит, что вторая изменилась сама, и перестаёт
    // доверять обеим.
    ImGui::SameLine(0.0f, ui.SpacingMD);
    CenterY(row);
    if (EditorIcons::IconOnlyButton("magnet", T("Snap to the step"), host.GizmoSnap()))
        host.GizmoSnap() = !host.GizmoSnap();
    ImGui::SameLine(0.0f, 0.0f);
    CenterY(row);
    // Стрелка рядом с магнитом: сам магнит включает привязку, стрелка открывает
    // её шаги. Два действия у одной кнопки не разделить — а шаг правят реже,
    // чем включают привязку, и прятать его за вторым щелчком правильно.
    if (ImGui::SmallButton("v" "###snapsteps")) ImGui::OpenPopup("##snapsteps");
    Sage::UI::Tooltip(T("Snap steps"));
    if (ImGui::BeginPopup("##snapsteps")) {
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat(T("Move"), &host.SnapMove(), 0.01f, 0.001f, 100.0f, "%.3f");
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat(T("Rotate"), &host.SnapRotate(), 0.5f, 0.1f, 180.0f, "%.1f°");
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat(T("Scale"), &host.SnapScale(), 0.01f, 0.001f, 10.0f, "%.3f");
        ImGui::EndPopup();
    }

    // --- СПРАВА: НАСТРОЙКИ ИГРЫ ---------------------------------------------
    //
    // КНОПКИ «ПРОЕКТ: ИМЯ» ЗДЕСЬ БОЛЬШЕ НЕТ. Она открывала меню, в котором не
    // было ничего своего: «открыть проект» стоит слева отдельной кнопкой,
    // сцену сохраняют кнопкой рядом с ней (и Ctrl+S), а всё остальное из того
    // меню есть в меню «Файл». Имя открытого проекта и сцены и так написаны —
    // в заголовке окна и в статус-баре, — а кнопка отнимала правую четверть
    // полосы ради строки, которую не нажимают.
    const float rightX = std::max(ImGui::GetItemRectMax().x - barMin.x + gap, rightStart);
    ImGui::SameLine(rightX);
    dividerAt(barMin.x + rightX - gap);
    CenterY(row);
    if (EditorIcons::IconOnlyButton("gear", T("Game Settings"),
                                    host.PanelVisible(EditorPanel::Settings))) {
        host.PanelVisible(EditorPanel::Settings) = !host.PanelVisible(EditorPanel::Settings);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

#include "PanelWindows.h"

#include <map>
#include <string>

#include "imgui_internal.h"

#include "EditorPrefs.h"
#include "Localization.h"

namespace panelwindows {
namespace {

// Панели, которые умеют жить своим окном, и их состояние по умолчанию.
//
// Размер здесь — не украшение: окно, открытое впервые, получает размер
// ImGui-панели, а он взят из дока и к отдельному окну отношения не имеет.
// Дерево иерархии в окне 1280x800 и холст интерфейса в окне 320x200 одинаково
// бесполезны.
struct Default {
    const char* Id;
    bool Detached;
    float Width;
    float Height;
};

// Редактор интерфейса — единственный, кто отделён ИЗНАЧАЛЬНО, и это следует
// из его же устройства (см. panels/UIEditorPanel.h): в нём и дерево, и холст,
// и свойства, и инструменты, то есть целая программа внутри вкладки. Втиснутый
// в центральный док рядом с вьюпортом, он отдаёт холсту полосу в треть экрана
// — а холст показывает КАДР ИГРЫ, по которому и верстают. Верстать интерфейс
// в окне размером с ладонь нельзя: ошибки раскладки видны только в натуральную
// величину.
const Default kDefaults[] = {
    // Панелей, отстыкованных ПО УМОЛЧАНИЮ, больше нет: вёрстка стала отдельным
    // рабочим пространством со своими панелями внутри главного окна, и
    // выталкивать её наружу незачем. Возможность никуда не делась — её включают
    // галочкой в меню «Окно», и проверка самотестом это делает явно.
    {"InterfaceViewport",  false, 1280.0f, 820.0f},
    {"InterfaceHierarchy", false,  380.0f, 720.0f},
    {"InterfaceInspector", false,  460.0f, 820.0f},
    {"InterfacePreview",   false,  900.0f, 560.0f},
    {"Viewport",   false, 1100.0f, 700.0f},
    {"Game",       false, 1100.0f, 700.0f},
    {"Hierarchy",  false,  380.0f, 720.0f},
    {"Inspector",  false,  460.0f, 820.0f},
    {"Lighting",   false,  460.0f, 720.0f},
    {"Assets",     false,  900.0f, 520.0f},
    {"Console",    false,  900.0f, 420.0f},
    {"Profiler",   false,  720.0f, 520.0f},
    {"Animation",  false, 1000.0f, 360.0f},
    // У ассетов, консоли и анимации СВОЁ ОКНО В КАЖДОМ ПРОСТРАНСТВЕ (см.
    // PanelWindowId.h), и состояние «отдельным окном» у каждого своё: окно
    // вёрстки, вытащенное на второй монитор, не обязано тащить туда же окно
    // сцены — это разные окна, хоть панель за ними и одна.
    {"AssetsUI",    false,  900.0f, 520.0f},
    {"ConsoleUI",   false,  900.0f, 420.0f},
    {"AnimationUI", false, 1000.0f, 360.0f},
};

struct State {
    bool Detached = false;  // чего хочет человек (галочка меню)
    bool Applied = false;   // что сделано на самом деле
    int Settle = 0;         // кадры, пока переезд не устоялся
    ImVec2 Size{0.0f, 0.0f};
};

std::map<std::string, State>& Panels() {
    static std::map<std::string, State> panels;
    return panels;
}

ImGuiID g_homeDock = 0;
// Окна, открытые подряд, иначе легли бы ровно друг на друга — и второе
// выглядело бы как «первое не открылось».
int g_cascade = 0;

std::string Key(const char* id) { return std::string("window.detached.") + id; }

const Default* FindDefault(const char* id) {
    for (const Default& d : kDefaults)
        if (std::string(d.Id) == id) return &d;
    return nullptr;
}

State& Get(const char* id) {
    auto& panels = Panels();
    auto it = panels.find(id);
    if (it != panels.end()) return it->second;

    State s;
    const Default* d = FindDefault(id);
    const bool fallback = d ? d->Detached : false;
    s.Detached = sage::editor::prefs::GetBool(Key(id), fallback);
    // Applied = Detached, а не false: при запуске состояние уже такое, каким
    // его помнит ini ImGui (позиции окон и доки лежат там). Считать иначе
    // значит устраивать «переезд» в первом же кадре и терять место, куда
    // человек это окно поставил.
    s.Applied = s.Detached;
    if (d) s.Size = ImVec2(d->Width, d->Height);
    return panels.emplace(id, s).first->second;
}

void Remember(const char* id, bool detached) {
    sage::editor::prefs::SetBool(Key(id), detached);
}

} // namespace

void Load() {
    for (const Default& d : kDefaults) Get(d.Id);
}

bool Detached(const char* id) { return Get(id).Detached; }

void SetDetached(const char* id, bool detached) {
    State& s = Get(id);
    if (s.Detached == detached) return;
    s.Detached = detached;
    Remember(id, detached);
}

void AttachAll() {
    for (const Default& d : kDefaults) SetDetached(d.Id, false);
}

void ResetToDefaults() {
    for (const Default& d : kDefaults) SetDetached(d.Id, d.Detached);
}

void SetHomeDock(ImGuiID dockspace) { g_homeDock = dockspace; }

ImGuiWindowFlags WindowFlags(const char* id) {
    // Спрашиваем НЕ состояние-намерение, а то, где окно на самом деле: в кадре
    // переезда намерение уже новое, а окна системы ещё нет, и заголовок исчез
    // бы раньше, чем появилась рамка.
    const ImGuiWindow* w = ImGui::FindWindowByName(id);
    if (!w || w->Viewport == nullptr) return 0;
    if (w->Viewport->ID == ImGui::GetMainViewport()->ID) return 0;
    if ((w->Viewport->Flags & ImGuiViewportFlags_NoDecoration) != 0) return 0;
    return ImGuiWindowFlags_NoTitleBar;
}

void Before(const char* id) {
    State& s = Get(id);

    // Класс окна ставится КАЖДЫЙ кадр, а не один раз при отрыве. ImGui решает
    // судьбу вьюпорта заново в каждом кадре, и стоит пропустить флаг, как окно
    // сольётся с главным при первом же наезде на него.
    ImGuiWindowClass cls;
    if (s.Detached) cls.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&cls);

    if (s.Detached == s.Applied) return;

    s.Applied = s.Detached;
    // Несколько кадров не сверяемся с реальностью: ImGui доводит переезд не
    // мгновенно (вьюпорт заводится, окно платформы создаётся, позиция
    // применяется), и сверка в этот промежуток прочитала бы промежуточное
    // состояние и тут же отменила бы приказ.
    s.Settle = 4;
    Remember(id, s.Detached);

    if (s.Detached) {
        // Из дока — наружу. Без этого окно осталось бы вкладкой в главном
        // окне: класс вьюпорта на пришвартованное окно не действует.
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        const ImGuiViewport* main = ImGui::GetMainViewport();
        const float step = 34.0f * (float)(g_cascade++ % 6);
        ImGui::SetNextWindowPos(ImVec2(main->Pos.x + 90.0f + step, main->Pos.y + 90.0f + step),
                                ImGuiCond_Always);
        if (s.Size.x > 0.0f && s.Size.y > 0.0f)
            ImGui::SetNextWindowSize(s.Size, ImGuiCond_Always);
    } else if (g_homeDock != 0) {
        // Обратно в главное окно — но НЕ поверх раскладки, если её только что
        // построил DockBuilder. Сброс раскладки разводит панели по своим узлам
        // (иерархию влево, инспектор вправо), а безусловный SetNextWindowDockID
        // тут же стащил бы их все в центральный узел: «сброс» складывал бы
        // редактор в одну стопку вкладок.
        ImGuiWindow* w = ImGui::FindWindowByName(id);
        const bool elsewhere = w && w->Viewport != nullptr &&
                               w->Viewport->ID != ImGui::GetMainViewport()->ID;
        if (w == nullptr || w->DockId == 0 || elsewhere)
            ImGui::SetNextWindowDockID(g_homeDock, ImGuiCond_Always);
    }
}

// КНОПКА «ВЕРНУТЬ В РЕДАКТОР» — ВНУТРИ САМОГО ОКНА.
//
// Пути назад мышью не было вовсе, и это не мелочь, а тупик. Заголовок у такой
// панели рисует СИСТЕМА (io.ConfigViewportsNoDecoration = false), а свой
// заголовок ImGui у неё погашен — то есть хватать нечего: за системный
// заголовок окно таскает система, ImGui про этот жест не знает и подсветку
// док-узлов не показывает. Оставалась галочка в меню «Окно», о которой надо
// ЗНАТЬ, — а человек видит окно, которое не возвращается, и читает это как
// поломку.
//
// Кнопка стоит в углу самого окна: там, где на неё смотрят, когда хотят его
// убрать. Отдельным окошком ImGui поверх панели, потому что панели рисуют себя
// сами и трогать каждую ради одной кнопки значит однажды забыть одну.
void DrawRedockButton(const char* id, ImGuiWindow* w) {
    ImGuiWindowClass cls;   // само окошко кнопки наружу не выносим
    cls.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&cls);
    ImGui::SetNextWindowViewport(w->Viewport->ID);
    const float pad = 6.0f;
    ImGui::SetNextWindowPos(ImVec2(w->Pos.x + w->Size.x - pad, w->Pos.y + pad), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const std::string name = std::string("##redock_") + id;
    if (ImGui::Begin(name.c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav)) {
        if (ImGui::SmallButton(T("Back to the editor"))) SetDetached(id, false);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Returns the panel to a tab of the main window"));
    }
    ImGui::End();
}

void After(const char* id) {
    State& s = Get(id);

    // Кнопка возврата — ДО ранних выходов ниже: она нужна ровно тогда, когда
    // окно уже стоит отдельно, а сверка состояния в этот момент вполне может
    // пропускать кадры (переезд, перетаскивание).
    if (ImGuiWindow* win = ImGui::FindWindowByName(id)) {
        if (win->WasActive && win->Viewport != nullptr &&
            win->Viewport->ID != ImGui::GetMainViewport()->ID) {
            DrawRedockButton(id, win);
        }
    }

    if (s.Settle > 0) { --s.Settle; return; }

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) return;
    // Пока окно ТАЩАТ мышью, ImGui держит его в отдельном временном вьюпорте —
    // это ещё не решение человека, а середина жеста. Сверяться здесь значит
    // ставить галочку на каждое подрагивание мыши над панелью.
    if (ctx->MovingWindow != nullptr) return;

    ImGuiWindow* w = ImGui::FindWindowByName(id);
    if (!w || !w->WasActive || w->Viewport == nullptr) return;

    const bool detached = w->Viewport->ID != ImGui::GetMainViewport()->ID;
    // Размер запоминается, пока окно отдельное: закрыть и открыть его снова
    // человек вправе ожидать таким же, каким оставил.
    if (detached) s.Size = w->Size;
    if (detached == s.Detached) return;

    s.Detached = detached;
    s.Applied = detached;
    Remember(id, detached);
}

} // namespace panelwindows

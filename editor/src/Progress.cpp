#include "Progress.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "imgui.h"

#include "EditorIcons.h"
#include "EditorTheme.h"
#include "Localization.h"

namespace sage::editor::progress {
namespace {

// Сколько секунд законченная задача ещё висит в углу. Меньше секунды человек
// просто не успевает прочитать, чем всё кончилось; больше трёх — карточки
// начинают копиться на экране.
constexpr double kKeepDoneSeconds = 2.5;

struct Entry {
    Id Ident = 0;
    Kind What = Kind::Background;
    std::string Title;
    std::string Note;
    float Fraction = -1.0f;
    bool Done = false;
    double DoneAt = 0.0;
};

struct State {
    std::mutex Mx;
    std::vector<Entry> Items;
    Id Next = 1;
};

State& S() {
    static State s;
    return s;
}

// Полоса. fraction < 0 — доля неизвестна, и полоса бежит: стоящая на нуле
// читается как «повисло», а «идёт, но сколько осталось — неизвестно» — это
// честный и частый ответ (распаковка, сеть).
void Bar(float fraction, float width, float height) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Input));
    const ImU32 fg = ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Accent));
    const float r = height * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), bg, r);
    if (fraction >= 0.0f) {
        const float w = std::clamp(fraction, 0.0f, 1.0f) * width;
        if (w > 1.0f) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), fg, r);
    } else {
        // Бегунок в треть длины ходит туда-обратно.
        const float seg = width * 0.33f;
        const double t = ImGui::GetTime() * 0.8;
        const float phase = (float)(t - std::floor(t));
        const float x = p.x + (width + seg) * phase - seg;
        const float a = std::max(x, p.x);
        const float b = std::min(x + seg, p.x + width);
        if (b > a) dl->AddRectFilled(ImVec2(a, p.y), ImVec2(b, p.y + height), fg, r);
    }
    ImGui::Dummy(ImVec2(width, height));
}

} // namespace

Id Begin(Kind kind, const std::string& title, const std::string& note) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    Entry e;
    e.Ident = s.Next++;
    e.What = kind;
    e.Title = title;
    e.Note = note;
    s.Items.push_back(std::move(e));
    return s.Items.back().Ident;
}

void Update(Id id, float fraction, const std::string& note) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    for (Entry& e : s.Items) {
        if (e.Ident != id) continue;
        e.Fraction = fraction;
        if (!note.empty()) e.Note = note;
        return;
    }
}

void End(Id id, const std::string& doneNote) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    for (Entry& e : s.Items) {
        if (e.Ident != id) continue;
        e.Done = true;
        e.Fraction = 1.0f;
        e.DoneAt = ImGui::GetTime();
        if (!doneNote.empty()) e.Note = doneNote;
        return;
    }
}

bool Blocked() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    for (const Entry& e : s.Items)
        if (e.What == Kind::Blocking && !e.Done) return true;
    return false;
}

std::vector<Task> Tasks() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    std::vector<Task> out;
    out.reserve(s.Items.size());
    for (const Entry& e : s.Items) out.push_back({e.Ident, e.What, e.Title, e.Note, e.Fraction, e.Done});
    return out;
}

void Clear() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.Mx);
    s.Items.clear();
}

void Draw() {
    State& s = S();
    std::vector<Entry> items;
    {
        std::lock_guard<std::mutex> lock(s.Mx);
        // Догоревшие убираем здесь же: отдельного «сборщика» для этого заводить
        // незачем, а расти список не должен.
        const double now = ImGui::GetTime();
        s.Items.erase(std::remove_if(s.Items.begin(), s.Items.end(),
                                     [&](const Entry& e) {
                                         return e.Done && now - e.DoneAt > kKeepDoneSeconds;
                                     }),
                      s.Items.end());
        items = s.Items;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    // --- Блокирующая: модальное окно по центру ------------------------------
    //
    // Первая незакрытая — она же и единственная: две одновременные работы, без
    // каждой из которых «дальше нельзя», означают, что одна из них на деле
    // фоновая.
    const Entry* blocking = nullptr;
    for (const Entry& e : items)
        if (e.What == Kind::Blocking && !e.Done) { blocking = &e; break; }

    // ЗАКРЫТИЕ — ДО ВЫХОДА ПО ПУСТОМУ СПИСКУ. Работа кончилась, список опустел,
    // а модальное окно осталось бы висеть навсегда: редактор, занятый ничем.
    if (!blocking && ImGui::IsPopupOpen("##progress_modal")) {
        if (ImGui::BeginPopupModal("##progress_modal", nullptr, ImGuiWindowFlags_NoTitleBar)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    if (items.empty()) return;

    if (blocking) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
        if (!ImGui::IsPopupOpen("##progress_modal")) ImGui::OpenPopup("##progress_modal");
        if (ImGui::BeginPopupModal("##progress_modal", nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextUnformatted(blocking->Title.c_str());
            ImGui::Spacing();
            Bar(blocking->Fraction, ImGui::GetContentRegionAvail().x, 6.0f);
            ImGui::Spacing();
            if (!blocking->Note.empty()) ImGui::TextDisabled("%s", blocking->Note.c_str());
            else ImGui::TextDisabled("%s", T("Working..."));
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
    }

    // --- Фоновые: карточки в правом нижнем углу -----------------------------
    //
    // Именно в углу и именно поверх всего: это не панель, её не закрывают и не
    // ищут — она сама попадается на глаза и сама уходит.
    constexpr float kCardW = 300.0f;
    float y = vp->Pos.y + vp->Size.y - 36.0f;   // над статусной строкой
    int index = 0;
    // КАРТОЧКА ОБЯЗАНА БЫТЬ КАРТОЧКОЙ: своя подложка, рамка и поля. Без них
    // текст ложится прямо на панель под собой — и читается как её содержимое,
    // а не как сообщение поверх.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::Color(EditorTheme::Role::Elevated));
    ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::Color(EditorTheme::Role::Line));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    for (const Entry& e : items) {
        if (e.What != Kind::Background) continue;
        const float h = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 40.0f;
        y -= h + 8.0f;
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - kCardW - 16.0f, y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(kCardW, h), ImGuiCond_Always);
        char id[32];
        std::snprintf(id, sizeof(id), "##progress_card_%d", index++);
        if (ImGui::Begin(id, nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking)) {
            if (e.Done) {
                EditorIcons::Inline("info", glm::vec3(0.45f, 0.85f, 0.55f));
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(e.Title.c_str());
            ImGui::Spacing();
            Bar(e.Done ? 1.0f : e.Fraction, ImGui::GetContentRegionAvail().x, 4.0f);
            ImGui::Spacing();
            if (!e.Note.empty()) ImGui::TextDisabled("%s", e.Note.c_str());
        }
        ImGui::End();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace sage::editor::progress

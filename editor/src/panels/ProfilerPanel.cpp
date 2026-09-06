#include "panels/ProfilerPanel.h"

#include <algorithm>
#include <cstdio>

#include "EditorTheme.h"
#include "sage/core/Profiler.h"
#include "../Localization.h"

using namespace sage::ui;
using namespace sage::ui::sui;

namespace {

// Цвет полосы по доле от самого дорогого участка: зелёный — дёшево, красный —
// это и есть узкое место. Цвет здесь не украшение: таблица из полутора десятков
// строк с числами читается глазами долго, а самая длинная красная полоса —
// мгновенно, и ровно она и есть ответ на вопрос, ради которого панель открыли.
UIColor BarColor(double share) {
    const float t = (float)std::min(std::max(share, 0.0), 1.0);
    return UIColor(0.25f + 0.70f * t, 0.75f - 0.45f * t, 0.30f, 0.85f);
}

UIColor RoleColor(EditorTheme::Role role) {
    const ImVec4 c = EditorTheme::Color(role);
    return UIColor(c.x, c.y, c.z, c.w);
}

constexpr float kNumberWidth = 66.0f;

} // namespace

void ProfilerPanel::SetShown(bool shown) {
    m_shown = shown;
    // Профилирование живёт ровно столько, сколько открыта панель.
    if (shown != m_wasEnabled) {
        sage::profile::SetEnabled(shown);
        m_wasEnabled = shown;
    }
}

void ProfilerPanel::Build(UIContext& ui, UIElement* root) {
    m_ui = &ui;
    root->Vertical(6.0f)->Padding(UIEdges::Uniform(8.0f));

    m_frame = ui.CreateIn<Label>(root, std::string());
    m_frame->SetName("Frame");
    m_frame->SetStretch(true, false);

    // Строка настроек: усреднение и пояснение, почему оно включено.
    UIElement* bar = ui.CreateIn<UIElement>(root);
    bar->SetName("Options");
    bar->SetHeight(24.0f);
    bar->SetStretch(true, false);
    bar->Horizontal(8.0f)->Padding(UIEdges::Uniform(0.0f));
    bar->Layout().Cross = UIAlign::Center;

    Checkbox* avg = ui.CreateIn<Checkbox>(bar, std::string(T("Average")), m_averaged);
    avg->SetHeight(22.0f);
    avg->Ensure<UITransform>().WidthMode = UISizeMode::Content;
    avg->OnToggle([this](bool on) { m_averaged = on; });

    Label* help = ui.CreateIn<Label>(bar, std::string("(?)"));
    help->SetColor(RoleColor(EditorTheme::Role::TextDim));
    // Подсказка — ДАННЫМИ узла: контекст покажет её сам после задержки. Своего
    // таймера и своего окна панели для этого заводить не нужно.
    UIInteraction& hi = help->Ensure<UIInteraction>();
    hi.TooltipKey =
        T("Per-frame numbers swing by tens of percent because of the OS scheduler.\n"
          "The 30-frame average is what you actually read.\n"
          "\n"
          "GPU time lags three frames behind: asking for it right away means waiting\n"
          "for the card, that is, creating the delay you are measuring.");

    // Шапка таблицы.
    UIElement* head = ui.CreateIn<UIElement>(root);
    head->SetName("Head");
    head->SetHeight(20.0f);
    head->SetStretch(true, false);
    head->Horizontal(8.0f)->Padding(UIEdges::Uniform(0.0f));
    head->Layout().Cross = UIAlign::Center;

    auto headCell = [&](const char* text, float width) {
        Label* l = ui.CreateIn<Label>(head, std::string(text));
        l->SetColor(RoleColor(EditorTheme::Role::TextDim));
        if (width > 0.0f) l->SetWidth(width);
        else l->SetStretch(true, false);
        return l;
    };
    headCell(T("Section"), 0.0f);
    headCell(T("CPU, ms"), kNumberWidth)->SetAlign(UITextAlign::Right, UITextVAlign::Center);
    headCell(T("GPU, ms"), kNumberWidth)->SetAlign(UITextAlign::Right, UITextVAlign::Center);
    Label* barHead = headCell("", 0.0f);
    barHead->SetWidth(140.0f);

    m_table = ui.CreateIn<ScrollView>(root);
    m_table->SetName("Sections");
    m_table->SetStretch(true, true);

    m_hint = ui.CreateIn<Label>(m_table->Content(), std::string());
    m_hint->SetColor(RoleColor(EditorTheme::Role::TextDim));
}

ProfilerPanel::Row& ProfilerPanel::EnsureRow(size_t index) {
    while (m_rows.size() <= index) {
        Row row;
        row.Box = m_ui->CreateIn<UIElement>(m_table->Content());
        row.Box->SetName("Row");
        row.Box->SetHeight(18.0f);
        row.Box->SetStretch(true, false);
        row.Box->Horizontal(8.0f)->Padding(UIEdges::Uniform(0.0f));
        row.Box->Layout().Cross = UIAlign::Center;

        row.Name = m_ui->CreateIn<Label>(row.Box, std::string());
        row.Name->SetStretch(true, false);
        row.Name->SetEllipsis(true);

        row.Cpu = m_ui->CreateIn<Label>(row.Box, std::string());
        row.Cpu->SetWidth(kNumberWidth);
        row.Cpu->SetAlign(UITextAlign::Right, UITextVAlign::Center);

        row.Gpu = m_ui->CreateIn<Label>(row.Box, std::string());
        row.Gpu->SetWidth(kNumberWidth);
        row.Gpu->SetAlign(UITextAlign::Right, UITextVAlign::Center);

        row.Bar = m_ui->CreateIn<ProgressBar>(row.Box, 0.0f);
        row.Bar->SetSize({140.0f, 10.0f});
        m_rows.push_back(row);
    }
    return m_rows[index];
}

void ProfilerPanel::Sync(float) {
    if (!m_ui || !m_shown) return;

    const bool gpu = sage::profile::GpuTimersAvailable();

    char frame[160];
    if (gpu) {
        std::snprintf(frame, sizeof(frame), "%s  %s",
                      T("Frame: CPU %.2f ms"), T("| GPU %.2f ms"));
        // Две отдельные строки формата склеивать нельзя: у каждой свой
        // аргумент. Собираем по очереди.
        char cpuPart[80], gpuPart[80];
        std::snprintf(cpuPart, sizeof(cpuPart), T("Frame: CPU %.2f ms"),
                      sage::profile::FrameCpuMs());
        std::snprintf(gpuPart, sizeof(gpuPart), T("| GPU %.2f ms"), sage::profile::FrameGpuMs());
        std::snprintf(frame, sizeof(frame), "%s  %s", cpuPart, gpuPart);
    } else {
        char cpuPart[80];
        std::snprintf(cpuPart, sizeof(cpuPart), T("Frame: CPU %.2f ms"),
                      sage::profile::FrameCpuMs());
        std::snprintf(frame, sizeof(frame), "%s  %s", cpuPart, T("| no GPU timers"));
    }
    m_frame->SetText(frame);

    const std::vector<sage::profile::Entry>& entries =
        m_averaged ? sage::profile::Average() : sage::profile::Frame();

    if (entries.empty()) {
        m_hint->SetVisible(true);
        m_hint->SetText(T("Timestamps are not ready yet — wait a few frames."));
        for (Row& r : m_rows) r.Box->SetVisible(false);
        return;
    }
    m_hint->SetVisible(false);

    // Масштаб полос — по самому дорогому участку ВЕРХНЕГО уровня. По максимуму
    // из всех подряд шкала задавалась бы вложенным участком, который по
    // определению не больше родителя, и полосы верхнего уровня упёрлись бы в
    // край.
    double maxMs = 0.0;
    for (const sage::profile::Entry& e : entries) {
        if (e.Depth != 0) continue;
        maxMs = std::max(maxMs, gpu ? std::max(e.CpuMs, e.GpuMs) : e.CpuMs);
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        const sage::profile::Entry& e = entries[i];
        Row& row = EnsureRow(i);
        row.Box->SetVisible(true);

        // Вложенность отступом: проход внутри прохода виден сразу, и «SSAO 2 мс»
        // не путается с «Пост-обработка 6 мс», в которые он входит.
        row.Box->Padding(UIEdges((float)e.Depth * 12.0f, 0.0f, 0.0f, 0.0f));
        row.Name->SetText(e.Name);

        char num[32];
        std::snprintf(num, sizeof(num), "%.2f", e.CpuMs);
        row.Cpu->SetText(num);
        if (gpu) {
            std::snprintf(num, sizeof(num), "%.2f", e.GpuMs);
            row.Gpu->SetText(num);
            row.Gpu->SetColor(RoleColor(EditorTheme::Role::Text));
        } else {
            row.Gpu->SetText("—");
            row.Gpu->SetColor(RoleColor(EditorTheme::Role::TextDim));
        }

        // Полоса строится по ТОЙ шкале, которая больше: узкое место может быть
        // на любой из двух, и показывать всегда CPU значило бы не увидеть кадр,
        // упирающийся в видеокарту.
        const double ms = gpu ? std::max(e.CpuMs, e.GpuMs) : e.CpuMs;
        const double share = maxMs > 0.0001 ? ms / maxMs : 0.0;
        row.Bar->SetValue((float)share);
        row.Bar->Ensure<UIProgress>().FillColor = BarColor(share);
    }
    for (size_t i = entries.size(); i < m_rows.size(); ++i) m_rows[i].Box->SetVisible(false);
}

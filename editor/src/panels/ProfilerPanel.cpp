#include "panels/ProfilerPanel.h"

#include <algorithm>
#include <vector>

#include "imgui.h"
#include "sage/core/Profiler.h"
#include "../Localization.h"

namespace {

// Цвет полосы по доле от самого дорогого участка: зелёный — дёшево, красный —
// это и есть узкое место. Цвет здесь не украшение: таблица из полутора десятков
// строк с числами читается глазами долго, а самая длинная красная полоса —
// мгновенно, и ровно она и есть ответ на вопрос, ради которого панель открыли.
ImVec4 BarColor(double share) {
    const float t = (float)std::min(std::max(share, 0.0), 1.0);
    return ImVec4(0.25f + 0.70f * t, 0.75f - 0.45f * t, 0.30f, 0.85f);
}

void DrawRow(const sage::profile::Entry& e, double maxMs, bool gpu) {
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    // Вложенность отступом: проход внутри прохода виден сразу, и «SSAO 2 мс»
    // не путается с «Пост-обработка 6 мс», в которые он входит.
    ImGui::Indent((float)e.Depth * 12.0f);
    ImGui::TextUnformatted(e.Name.c_str());
    ImGui::Unindent((float)e.Depth * 12.0f);

    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%.2f", e.CpuMs);

    ImGui::TableSetColumnIndex(2);
    if (gpu) ImGui::Text("%.2f", e.GpuMs);
    else ImGui::TextDisabled("—");

    ImGui::TableSetColumnIndex(3);
    // Полоса строится по ТОЙ шкале, которая больше: узкое место может быть на
    // любой из двух, и показывать всегда CPU значило бы не увидеть кадр,
    // упирающийся в видеокарту.
    const double ms = gpu ? std::max(e.CpuMs, e.GpuMs) : e.CpuMs;
    const double share = maxMs > 0.0001 ? ms / maxMs : 0.0;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, BarColor(share));
    ImGui::ProgressBar((float)share, ImVec2(-1.0f, 0.0f), "");
    ImGui::PopStyleColor();
}

} // namespace

void ProfilerPanel::Draw(bool* open) {
    // Профилирование живёт ровно столько, сколько открыта панель.
    const bool wantEnabled = open && *open;
    if (wantEnabled != m_wasEnabled) {
        sage::profile::SetEnabled(wantEnabled);
        m_wasEnabled = wantEnabled;
    }
    if (!wantEnabled) return;

    if (!ImGui::Begin(T("Profiler" "###Profiler"), open)) {
        ImGui::End();
        return;
    }

    const bool gpu = sage::profile::GpuTimersAvailable();

    ImGui::Text(T("Frame: CPU %.2f ms"), sage::profile::FrameCpuMs());
    if (gpu) {
        ImGui::SameLine();
        ImGui::Text(T("| GPU %.2f ms"), sage::profile::FrameGpuMs());
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("| no GPU timers"));
    }

    ImGui::Checkbox(T("Average"), &m_averaged);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s", T("Per-frame numbers swing by tens of percent because of the OS scheduler.\n"
              "The 30-frame average is what you actually read.\n"
              "\n"
              "GPU time lags three frames behind: asking for it right away means waiting\n"
              "for the card, that is, creating the delay you are measuring."));
    }

    const std::vector<sage::profile::Entry>& entries =
        m_averaged ? sage::profile::Average() : sage::profile::Frame();

    if (entries.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", T("Timestamps are not ready yet — wait a few frames."));
        ImGui::End();
        return;
    }

    // Масштаб полос — по самому дорогому участку ВЕРХНЕГО уровня. По максимуму
    // из всех подряд шкала задавалась бы вложенным участком, который по
    // определению не больше родителя, и полосы верхнего уровня упёрлись бы в
    // край.
    double maxMs = 0.0;
    for (const sage::profile::Entry& e : entries) {
        if (e.Depth != 0) continue;
        maxMs = std::max(maxMs, gpu ? std::max(e.CpuMs, e.GpuMs) : e.CpuMs);
    }

    ImGui::Separator();
    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##profile", 4, flags)) {
        ImGui::TableSetupColumn(T("Section"), ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn(T("CPU, ms"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(T("GPU, ms"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableHeadersRow();
        for (const sage::profile::Entry& e : entries) DrawRow(e, maxMs, gpu);
        ImGui::EndTable();
    }

    ImGui::End();
}

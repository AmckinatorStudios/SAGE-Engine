#include "panels/ProfilerPanel.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "sage/core/SystemScheduler.h"
#include "sage/jobs/JobSystem.h"
#include "../Localization.h"

// Вкладка «Потоки» профилировщика: что делает система задач прямо сейчас.
//
// Вопросы, на которые она отвечает, — по убыванию частоты: «все ли ядра
// заняты или кадр ждёт один поток» (занятость потоков и шкала времени),
// «какая подсистема ест потоки» (таблица подсистем), «почему ждём» (очереди,
// замки), «что сломалось» (зависшие задачи и ошибки), «почему эти системы не
// идут параллельно» (граф кадра).
namespace {

using namespace sage::jobs;

const char* CategoryLabel(Category c) {
    switch (c) {
        case Category::General: return T("General");
        case Category::ECS: return T("ECS systems");
        case Category::Render: return T("Rendering");
        case Category::Physics: return T("Physics");
        case Category::Animation: return T("Animation");
        case Category::Audio: return T("Audio");
        case Category::Scripts: return T("Scripts");
        case Category::Resources: return T("Resources");
        case Category::AssetImport: return T("Asset import");
        case Category::IO: return T("Input/output");
        case Category::AI: return T("AI");
        case Category::Editor: return T("Editor");
        case Category::Network: return T("Network");
        case Category::Game: return T("Game");
        default: return "?";
    }
}

const char* LaneLabel(Lane l) {
    switch (l) {
        case Lane::Worker: return T("frame work");
        case Lane::Background: return T("long jobs");
        case Lane::IO: return T("input/output");
        case Lane::Main: return T("main thread");
        case Lane::Render: return T("render thread");
        default: return "?";
    }
}

// Цвет подсистемы на шкале — устойчивый (по номеру), чтобы глаз запоминал.
ImU32 CategoryColor(Category c) {
    static const ImU32 palette[] = {
        IM_COL32(150, 150, 150, 230), IM_COL32(90, 160, 230, 230), IM_COL32(230, 140, 60, 230),
        IM_COL32(110, 200, 110, 230), IM_COL32(200, 110, 200, 230), IM_COL32(80, 200, 200, 230),
        IM_COL32(230, 210, 80, 230),  IM_COL32(170, 120, 80, 230),  IM_COL32(230, 100, 120, 230),
        IM_COL32(120, 120, 220, 230), IM_COL32(160, 200, 90, 230),  IM_COL32(200, 160, 120, 230),
        IM_COL32(100, 170, 150, 230), IM_COL32(220, 120, 200, 230),
    };
    return palette[(size_t)c % (sizeof(palette) / sizeof(palette[0]))];
}

ImVec4 LoadColor(double share) {
    const float t = (float)std::min(std::max(share, 0.0), 1.0);
    return ImVec4(0.25f + 0.70f * t, 0.75f - 0.45f * t, 0.30f, 0.85f);
}

} // namespace

void ProfilerPanel::DrawThreadsTab(sage::SystemScheduler* systems) {
    sage::JobSystem& js = sage::JobSystem::Get();
    const JobSystemStats st = js.Stats();
    if (!st.Running) {
        ImGui::TextDisabled("%s", T("The job system is not running."));
        return;
    }

    // --- Сводка ----------------------------------------------------------
    const double now = js.NowMs();
    if (m_lastStatsMs > 0.0 && now > m_lastStatsMs) {
        const double rate = (double)(st.Completed - std::min(st.Completed, m_lastCompleted)) * 1000.0 /
                            (now - m_lastStatsMs);
        m_jobsPerSecond = m_jobsPerSecond * 0.9 + rate * 0.1;
    }
    m_lastCompleted = st.Completed;
    m_lastStatsMs = now;

    ImGui::Text(T("Workers: %d (+main) | I/O threads: %d | long jobs at once: %d"), st.Workers, st.IoThreads,
                st.BackgroundLimit);
    ImGui::Text(T("Running: %d | waiting for dependencies: %d | jobs per second: %.0f"), st.Running_,
                st.WaitingOnDeps, m_jobsPerSecond);
    ImGui::Text(T("Queued: frame %d, long %d, I/O %d, main %d, render %d"), st.Queued[(int)Lane::Worker],
                st.Queued[(int)Lane::Background], st.Queued[(int)Lane::IO], st.Queued[(int)Lane::Main],
                st.Queued[(int)Lane::Render]);
    const double stealShare = st.StealAttempts ? (double)st.Steals / (double)st.StealAttempts : 0.0;
    ImGui::Text(T("Stolen: %llu (%.0f%% of attempts) | done while waiting: %llu"),
                (unsigned long long)st.Steals, stealShare * 100.0, (unsigned long long)st.HelpedWhileWaiting);
    ImGui::Text(T("Main thread waited for jobs: %llu times, %.1f ms total"),
                (unsigned long long)st.MainThreadWaits, st.MainThreadWaitMs);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("The main thread should not wait for jobs: every millisecond here is a\n"
                                  "millisecond of the frame. Take results with IsReady / ThenOnMain instead."));
    if (ImGui::Button(T("Reset counters"))) {
        js.ResetStats();
        m_lastCompleted = 0;
        m_jobsPerSecond = 0.0;
    }

    // --- Зависшие и ошибки — первыми: если они есть, всё остальное потом ----
    if (!st.Hung.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", T("Hung jobs (running longer than the limit):"));
        for (const HungJob& h : st.Hung)
            ImGui::BulletText(T("%s — %.0f ms on %s (%s, %s)"), h.Name.c_str(), h.RunningMs, h.Thread.c_str(),
                              CategoryLabel(h.Cat), LaneLabel(h.InLane));
    }
    if (!st.Errors.empty() &&
        ImGui::CollapsingHeader(T("Job errors"), ImGuiTreeNodeFlags_None)) {
        for (auto it = st.Errors.rbegin(); it != st.Errors.rend(); ++it)
            ImGui::BulletText(T("%.1f s  %s (%s): %s"), it->AtSeconds, it->Job.c_str(), CategoryLabel(it->Cat),
                              it->Message.c_str());
    }

    // --- Потоки ------------------------------------------------------------
    if (ImGui::CollapsingHeader(T("Threads"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
        if (ImGui::BeginTable("##jobThreads", 5, flags)) {
            ImGui::TableSetupColumn(T("Thread"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn(T("Load"), ImGuiTableColumnFlags_WidthStretch, 0.3f);
            ImGui::TableSetupColumn(T("Jobs"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn(T("Stolen"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn(T("Running now"), ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableHeadersRow();
            for (const ThreadStats& t : st.Threads) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(t.Kind == Lane::Main ? T("Main thread") : t.Name.c_str());
                ImGui::TableSetColumnIndex(1);
                char pct[32];
                std::snprintf(pct, sizeof(pct), "%.0f%%", t.Utilization * 100.0);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, LoadColor(t.Utilization));
                ImGui::ProgressBar((float)t.Utilization, ImVec2(-1.0f, 0.0f), pct);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", (unsigned long long)t.Executed);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%llu", (unsigned long long)t.Stolen);
                ImGui::TableSetColumnIndex(4);
                if (t.CurrentJob.empty()) ImGui::TextDisabled("%s", T("idle"));
                else ImGui::Text(T("%s, %.1f ms"), t.CurrentJob.c_str(), t.CurrentJobMs);
            }
            ImGui::EndTable();
        }
    }

    // --- Шкала времени -----------------------------------------------------
    if (ImGui::CollapsingHeader(T("Timeline"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox(T("Record"), &m_timeline)) js.SetTracing(m_timeline);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat(T("Window, ms"), &m_timelineWindowMs, 10.0f, 500.0f, "%.0f");
        if (m_timeline) DrawTimeline();
    }

    // --- Подсистемы --------------------------------------------------------
    if (ImGui::CollapsingHeader(T("Subsystems"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
        if (ImGui::BeginTable("##jobCats", 6, flags)) {
            ImGui::TableSetupColumn(T("Subsystem"), ImGuiTableColumnFlags_WidthStretch, 0.3f);
            ImGui::TableSetupColumn(T("Jobs"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn(T("Avg, ms"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn(T("Max, ms"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn(T("Queue, ms"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn(T("Failed / canceled"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();
            for (const CategoryStats& c : st.Categories) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(CategoryLabel(c.Cat));
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%llu", (unsigned long long)c.Executed);
                const double n = c.Executed ? (double)c.Executed : 1.0;
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.3f", c.ExecMs / n);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f", c.MaxExecMs);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.3f", c.QueueMs / n);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%llu / %llu", (unsigned long long)c.Failed, (unsigned long long)c.Canceled);
            }
            ImGui::EndTable();
        }
    }

    // --- Замки -------------------------------------------------------------
    if (ImGui::CollapsingHeader(T("Locks"))) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
        if (ImGui::BeginTable("##jobLocks", 5, flags)) {
            ImGui::TableSetupColumn(T("Lock"), ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableSetupColumn(T("Taken"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn(T("Contended"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn(T("Waited, ms"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn(T("Longest, ms"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();
            int shown = 0;
            for (const sage::sync::LockStats& l : st.Locks) {
                if (shown++ >= 20) break;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(l.Name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%llu", (unsigned long long)l.Acquisitions);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", (unsigned long long)l.Contended);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f", l.WaitMs);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.3f", l.MaxWaitMs);
            }
            ImGui::EndTable();
        }
    }

    // --- Граф систем кадра --------------------------------------------------
    if (systems && ImGui::CollapsingHeader(T("Frame systems graph"))) {
        const auto graph = systems->Graph();
        std::unordered_map<std::string, float> ms;
        std::unordered_map<std::string, bool> onWorker;
        for (const auto& t : systems->LastTimings()) {
            ms[t.Name] = t.Milliseconds;
            onWorker[t.Name] = t.OnWorker;
        }
        if (!systems->GraphError().empty())
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "%s", systems->GraphError().c_str());
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
        if (ImGui::BeginTable("##sysGraph", 5, flags)) {
            ImGui::TableSetupColumn(T("System"), ImGuiTableColumnFlags_WidthStretch, 0.25f);
            ImGui::TableSetupColumn(T("Stage"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn(T("Where"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn(T("ms"), ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn(T("Waits for"), ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableHeadersRow();
            for (const auto& n : graph) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(n.Name.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", n.Access.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(sage::StageName(n.InStage));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(onWorker[n.Name] ? T("workers") : T("main thread"));
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f", ms[n.Name]);
                ImGui::TableSetColumnIndex(4);
                std::string deps;
                for (int d : n.DependsOn) {
                    if (!deps.empty()) deps += ", ";
                    deps += graph[(size_t)d].Name;
                }
                if (deps.empty()) ImGui::TextDisabled("%s", T("nobody — starts at once"));
                else ImGui::TextUnformatted(deps.c_str());
            }
            ImGui::EndTable();
        }
    }
}

void ProfilerPanel::DrawTimeline() {
    sage::JobSystem& js = sage::JobSystem::Get();
    const std::vector<TraceEvent> events = js.Trace(m_timelineWindowMs);
    const JobSystemStats st = js.Stats();
    const int rows = (int)st.Threads.size();
    if (rows == 0) return;
    const float rowH = 16.0f;
    const float labelW = 110.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(100.0f, ImGui::GetContentRegionAvail().x);
    const float height = rowH * (float)rows + 4.0f;
    ImGui::InvisibleButton("##timeline", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double end = js.NowMs();
    const double start = end - m_timelineWindowMs;
    const float barW = width - labelW;

    // Строки потоков: индекс потока в событии → строка таблицы.
    std::unordered_map<int, int> rowOf;
    for (int r = 0; r < rows; ++r) {
        const ThreadStats& t = st.Threads[(size_t)r];
        const int idx = t.Kind == Lane::Main ? -1 : r - 1;
        rowOf[idx] = r;
        const float y = origin.y + rowH * (float)r;
        dl->AddRectFilled(ImVec2(origin.x + labelW, y), ImVec2(origin.x + width, y + rowH - 2.0f),
                          IM_COL32(255, 255, 255, 12));
        dl->AddText(ImVec2(origin.x, y), ImGui::GetColorU32(ImGuiCol_Text),
                    t.Kind == Lane::Main ? T("Main thread") : t.Name.c_str());
    }
    const TraceEvent* under = nullptr;
    for (const TraceEvent& e : events) {
        auto it = rowOf.find(e.Thread);
        if (it == rowOf.end()) continue;
        const float x0 = origin.x + labelW + (float)((std::max(e.StartMs, start) - start) / m_timelineWindowMs) * barW;
        const float x1 = origin.x + labelW + (float)((std::min(e.EndMs, end) - start) / m_timelineWindowMs) * barW;
        const float y = origin.y + rowH * (float)it->second;
        const ImVec2 a(x0, y + 1.0f), b(std::max(x1, x0 + 1.0f), y + rowH - 3.0f);
        dl->AddRectFilled(a, b, CategoryColor(e.Cat));
        if (hovered && mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y) under = &e;
    }
    if (under)
        ImGui::SetTooltip(T("%s\n%s, %.3f ms"), under->Name.c_str(), CategoryLabel(under->Cat),
                          under->EndMs - under->StartMs);
}

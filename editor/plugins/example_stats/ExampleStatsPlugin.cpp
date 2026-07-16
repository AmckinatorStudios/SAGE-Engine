#include <cstdio>
#include <deque>
#include <vector>

#include "imgui.h"
#include "PluginAPI.h"

// ============================================================================
//  example_stats — плагин-пример редактора SAGE.
//
//  Панель "Plugin: Stats" с графиком времени кадра (мс) и текущим выделенным
//  объектом сцены (через узкий EditorPluginContext, без доступа к Scene&).
//  Доказывает полный жизненный цикл плагина: OnLoad -> OnUpdate/OnImGui
//  каждый кадр -> OnUnload, собранный отдельной динамической библиотекой и
//  загруженный PluginManager'ом из plugins/ рядом с SageEditor.
// ============================================================================
class ExampleStatsPlugin : public SageEditorPlugin {
public:
    const char* Name() const override { return "Example Stats"; }

    void OnLoad(EditorPluginContext& ctx) override {
        m_ctx = &ctx;
        m_ctx->Log("example_stats: загружен");
    }

    void OnUnload() override {
        if (m_ctx) m_ctx->Log("example_stats: выгружается");
        m_ctx = nullptr;
    }

    void OnUpdate(float dt) override {
        m_frameTimesMs.push_back(dt * 1000.0f);
        while (m_frameTimesMs.size() > 120) m_frameTimesMs.pop_front();
    }

    void OnImGui() override {
        ImGui::Begin("Plugin: Stats");

        if (!m_frameTimesMs.empty()) {
            std::vector<float> values(m_frameTimesMs.begin(), m_frameTimesMs.end());
            float avg = 0.0f;
            for (float v : values) avg += v;
            avg /= static_cast<float>(values.size());

            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "avg %.2f ms", avg);
            ImGui::PlotLines("Frame time", values.data(), static_cast<int>(values.size()),
                              0, overlay, 0.0f, 33.0f, ImVec2(0, 60));
        }

        if (m_ctx) {
            const char* selected = m_ctx->SelectedEntityName();
            ImGui::Text("Selected: %s", (selected && selected[0]) ? selected : "(none)");
        }

        ImGui::End();
    }

private:
    EditorPluginContext* m_ctx = nullptr;
    std::deque<float> m_frameTimesMs;
};

extern "C" SageEditorPlugin* CreateSageEditorPlugin() { return new ExampleStatsPlugin(); }
extern "C" void DestroySageEditorPlugin(SageEditorPlugin* plugin) { delete plugin; }

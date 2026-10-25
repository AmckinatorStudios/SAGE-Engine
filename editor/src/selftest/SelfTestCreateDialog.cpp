#include "SelfTestCommon.h"
#include "CreateAssetDialog.h"
#include "imgui_internal.h"

// ---------------------------------------------------------------------------
// ОКНО «НОВЫЙ МАТЕРИАЛ» ДЕРЖИТ ШИРИНУ.
//
// Окно подгоняется под содержимое (AlwaysAutoResize), а поле имени тянулось
// «до края окна» — то есть мерило себя окном, а окно мерило себя им. Каждый
// кадр оба становились уже, и через секунду от окна оставалась полоска.
// Видно это только по живым кадрам, поэтому окно открывается и держится
// открытым несколько десятков кадров. Затем материал открывается в инспекторе:
// поля там тянулись на долю окна и обрезали подписи («сжатый» редактор).
// ---------------------------------------------------------------------------
void EditorLayer::CheckCreateDialogFrame() {
    if (m_createDialogChecked || !std::getenv("SAGE_EDITOR_SELFTEST") || !m_tweenChecked) return;
    namespace ca = sage::editor::createasset;
    const int step = m_createDialogStep++;
    if (step == 0) {
        ca::Request r;
        r.Kind = ca::What::Material;
        ca::Open(r);
        return;
    }
    if (step < 60) return;   // окно живёт кадры подряд: сжатие копится
    if (step == 60) {
        m_createDialogWidth = ca::WindowWidth();
        ca::RequestClose();
        // Созданный материал — в инспектор: его поля не должны обрезать
        // подписи (поле во всю долю окна оставляло подписи огрызок).
        fs::path made;
        std::string err;
        if (m_project.Loaded() &&
            AssetsPanel::CreateAsset(AssetsPanel::CreateKind::Material, "SelftestWidth", m_project.AssetsDir(), made, err)) {
            m_assets.Select(made.string());
            m_inspector.ForgetMaterialLabelsFit();
        }
        return;
    }
    if (step < 70) {
        if (ImGuiWindow* w = ImGui::FindWindowByID(ImHashStr("Inspector"))) ImGui::FocusWindow(w);
        return;
    }
    const float want = ImGui::GetFontSize() * 20.0f;
    const bool fit = m_inspector.MaterialLabelsFit();
    m_createDialogChecked = true;
    if (m_createDialogWidth >= want && fit)
        LOG_INFO("Editor") << "CREATE_DIALOG: OK — окно «Новый материал» держит ширину (" << (int)m_createDialogWidth
                           << " px), подписи полей материала видны целиком";
    else
        LOG_ERROR("Editor") << "CREATE_DIALOG: FAIL — "
                            << (fit ? "окно «Новый материал» сжалось до " + std::to_string((int)m_createDialogWidth) + " px"
                                    : std::string("поля материала обрезают подписи (или не нарисованы)"));
}

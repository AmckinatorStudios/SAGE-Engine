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
    // По очереди с окнами AI Graph: та проверка запускает Play и выводит окна
    // вперёд, а модальное окно, открытое в тот же кадр, закрывается.
    if (m_aiGraphFrameStep >= 0) return;
    namespace ca = sage::editor::createasset;
    // Отсчёт — с десятого кадра после окон AI Graph: они только что
    // остановили Play и вернули фокус редактору.
    const int step = m_createDialogStep++ - 10;
    if (step < 0) return;
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
    // Окно «Новый скрипт»: три вида в двух разделах, высота — в пределах
    // экрана при любом разрешении (лишнее прокручивается внутри).
    if (step == 70) {
        ca::Request r;
        r.Kind = ca::What::Script;
        ca::Open(r);
        return;
    }
    if (step < 90) return;
    if (step == 90) {
        const float h = ca::WindowHeight();
        const float limit = ImGui::GetMainViewport()->WorkSize.y * 0.85f + 1.0f;
        m_createScriptFits = h > 0.0f && h <= limit && ca::WindowWidth() <= ImGui::GetMainViewport()->WorkSize.x;
        m_createScriptHeight = h;
        ca::RequestClose();
        return;
    }
    // ОКНО «НАСТРОЙКИ» С ПОИСКОМ: строка поиска раскладывает находки по
    // разделам, двигая курсор, — и в ImGui 1.92 это давало красное «Code uses
    // SetCursorPos() to extend window boundaries» поверх окна. Ошибки ImGui
    // кадра считаются здесь, пока окно рисуется с запросом «о».
    if (step == 95) {
        m_panels[EditorPanel::Settings] = true;
        m_settingsPanel.SetSearch("\xD0\xBE");   // «о»
        m_settingsErrors = 0;
        return;
    }
    if (step < 115) {
        if (step > 97) m_settingsErrors += ImGui::GetCurrentContext()->ErrorCountCurrentFrame;
        return;
    }
    if (step == 115) {
        m_settingsPanel.SetSearch("");
        m_panels[EditorPanel::Settings] = false;
        if (m_settingsErrors == 0) LOG_INFO("Editor") << "SETTINGS_SEARCH: OK — поиск в окне «Настройки» без ошибок ImGui";
        else LOG_ERROR("Editor") << "SETTINGS_SEARCH: FAIL — ошибок ImGui в окне «Настройки» с поиском: " << m_settingsErrors;
        return;
    }
    const float want = ImGui::GetFontSize() * 20.0f;
    const bool fit = m_inspector.MaterialLabelsFit();
    m_createDialogChecked = true;
    if (!m_createScriptFits)
        LOG_ERROR("Editor") << "CREATE_DIALOG: FAIL — окно «Новый скрипт» не помещается в экран (высота "
                            << (int)m_createScriptHeight << " px, экран "
                            << (int)ImGui::GetMainViewport()->WorkSize.y << " px)";
    else if (m_createDialogWidth >= want && fit)
        LOG_INFO("Editor") << "CREATE_DIALOG: OK — окно «Новый материал» держит ширину (" << (int)m_createDialogWidth
                           << " px), подписи полей материала видны целиком";
    else
        LOG_ERROR("Editor") << "CREATE_DIALOG: FAIL — "
                            << (fit ? "окно «Новый материал» сжалось до " + std::to_string((int)m_createDialogWidth) + " px"
                                    : std::string("поля материала обрезают подписи (или не нарисованы)"));
}

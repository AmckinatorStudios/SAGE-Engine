#include "SelfTestCommon.h"


// ============================================================================
//  Self-test (SAGE_EDITOR_SELFTEST=1, headless CI)
// ============================================================================

// Headless-проверка ядра редактора без UI-кликов (модалки недоступны в CI):
// проект -> сцена -> undo/redo -> ассеты -> материалы -> камера -> Play.
// Результат — строкой SELFTEST: PASS/FAIL в лог.
void EditorLayer::RunSelfTest() {
    size_t before = m_scene->Count();
    std::string err;
    bool ok = true;

    // Подтверждения опасных действий выключаем на время прогона: диалог ждёт
    // человека, а здесь его нет. Это не обход проверки — сама работа
    // подтверждений проверяется ниже отдельным шагом, в обоих состояниях
    // галочки «больше не спрашивать».
    m_confirm.SetSuppressed("delete-entity", true);

    std::error_code ec;
    fs::remove_all("selftest_project", ec); // от прошлого прогона
    if (!CreateProject(".", "selftest_project", DefaultProjectTemplate(), err)) {
        LOG_ERROR("Editor") << "SELFTEST: create project failed: " << err;
        ok = false;
    }
    if (ok) {
        before = m_scene->Count(); // CreateProject пересоздал демо-сцену
        fs::path scenePath = m_project.ScenesDir() / "selftest.sage";
        if (!SaveSceneToFile(scenePath)) ok = false;
        if (ok) {
            NewScene(ProjectTemplateKind::Empty); // пустая сцена — убеждаемся, что загрузка реально восстанавливает
            if (!LoadSceneFromFile(scenePath)) ok = false;
        }
        if (ok && m_scene->Count() != before) {
            LOG_ERROR("Editor") << "SELFTEST: entity count mismatch: saved " << before
                                << ", loaded " << m_scene->Count();
            ok = false;
        }
    }


    // Дальше — блоки проверок по областям. Каждый возвращает свой итог, и
    // ВСЕ они выполняются независимо от того, упал ли предыдущий: прогон
    // должен показать ВСЕ поломки разом, а не первую. Раньше это была одна
    // функция на две тысячи строк с общим флагом ok, и половина проверок
    // молча пропускалась после первой же ошибки.
    if (ok) {
        ok = SelfTestProjectAndAssets() && ok;
        ok = SelfTestSceneAndPlay() && ok;
        ok = SelfTestSystems() && ok;
        ok = SelfTestSelection() && ok;
        ok = SelfTestTools() && ok;
        ok = SelfTestRenderStability() && ok;
        ok = SelfTestPickerAndPrefabs() && ok;
        ok = SelfTestPlugins() && ok;
        ok = SelfTestPanelRegistry() && ok;
    }

    if (ok) LOG_INFO("Editor") << "SELFTEST: PASS (project + scene + undo/redo + assets + "
                               << "materials + camera + light + primitives + environment + icons + folder-state + asset-slots + file-dialog + asset-scope + code-apps + script-reload + model-pack + anim-clips + model-skeleton + texture-set + object-catalog + rect-select + pause + import-any + icons + folders + mesh-component + markers + capsule + step + hidden + preview-cache + audio-preview + inspector-lock + build + "
                               << "recent + dirty + play + physics + animation + config + particles + "
                               << "culling + duplicate + hierarchy + multiselect + prefab + presets + GI + "
                               << "models + prefab-api + confirm + pick + tools + formats + ortho + "
                               << "scene-save + assets-root + scale-lock + viewport-tools + import + asset-refs + model-material + prefab-cover + drag-drop + settings-live + "
                               << "project-scripts + broken-scripts + replay + error-flood + panels + sidecars + "
                               << "all-components-roundtrip + ui-layout-tools + panel-flags + multi-window + editor-prefs + material-assign + "
                               << "vars-refs-events + prefab-refs + templates + themes + input-mapping + audio + "
                               << "render-stability + camera-preview + ui-backdrop + property-anim + anim-owner + editor-font + config-dir + lights-are-objects + interface-context + interface-open-button + cover-aspect + preview-for-every-asset + post-on-camera + build-needs-scene + gizmo-after-post + ui-type-l10n + play-in-interface + nine-slice + folder-marks + scene-switch + start-scene + debug-draw + model-import + object-picker + prefab-drop + plugins + panel-registry, "
                               << before << " entities)";
    else LOG_ERROR("Editor") << "SELFTEST: FAIL";
}

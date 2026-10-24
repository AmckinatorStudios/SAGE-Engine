#include "../AssetOpen.h"
#include "../CreateAssetDialog.h"
#include "../DragSet.h"
#include "../PanelWindowId.h"
#include "../PanelWindows.h"
#include "AssetsPanel.h"
#include "FsCache.h"
#include "ModelImportDialog.h"
#include "../Progress.h"

#include <chrono>
#include <future>
#include "../FolderColors.h"
#include "ui/UI.h"
#include "EditorTheme.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_internal.h" // BeginDragDropTargetCustom: объект на всё окно сетки

#include "EditorHost.h"
#include "../AssetSlot.h"
#include "../EditorIcons.h"
#include "../HotkeyScope.h"
#include "../SceneCover.h"
#include "../Thumbnails.h"
#include "../AssetCovers.h"

#include "sage/assets/import/Convert.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Zip.h"
#include "Project.h"
#include "sage/core/Log.h"
#include "sage/ecs/LightSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"
#include "../Localization.h"

#include "AssetsPanelDetail.h"

namespace fs = std::filesystem;



namespace covers = sage::editor::covers;
using namespace sage::editor::assetspanel_detail;

// Кнопка «Импорт…» и фоновое внесение файлов в проект.
void AssetsPanel::DrawImportButton(EditorHost& host) {
    Project& project = host.CurrentProject();

    if (EditorIcons::Button("open", T("Import..."))) {
        FileBrowser::Config c;
        c.Title = T("Bring into the project");
        // ФАЙЛ, ПАПКА ИЛИ АРХИВ — одной кнопкой. Скачивают по-разному: модель
        // одним файлом, набор текстур папкой, ассет с маркетплейса архивом.
        // Кнопка, которая берёт только файл, на двух из трёх случаев отвечает
        // «выберите файл» — и человек идёт распаковывать и перетаскивать руками
        // ровно то, что программа сделала бы за секунду.
        c.Mode = FileBrowser::PickMode::OpenAny;
        // Пусто — показывать всё: в проект вносят и модели, и картинки, и звук,
        // и чужие скрипты, а перечислять их фильтром значит однажды забыть
        // формат, который движок уже понимает.
        c.FilterLabel = T("All files");
        m_importBrowser.Open(c);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Copy an outside file, folder or .zip into the current project folder.\n"
                                  "A model moves together with its .mtl/.bin files and textures;\n"
                                  "an archive is unpacked into a folder of its own."));
    }

    if (!m_importBrowser.Draw()) return;
    if (m_import.valid()) return;   // одно внесение за раз: два потока в одну папку — гонка

    // ВНЕСЕНИЕ ИДЁТ ФОНОМ, А НА ЭКРАНЕ — ПОЛОСА.
    //
    // Папка ассетов с маркетплейса это сотни файлов и сотни мегабайт: копируя
    // их внутри кадра, редактор замирал на минуты — ни кадра, ни курсора, ни
    // ответа на вопрос «оно вообще работает?». Полоса ВАЖНАЯ (модальная):
    // папка проекта в это время меняется под ногами, и щёлкать по ней нельзя
    // (см. Progress.h).
    namespace progress = sage::editor::progress;
    const fs::path source = m_importBrowser.Result();
    m_importTask = progress::Begin(progress::Kind::Blocking,
                                   std::string(T("Bringing into the project: ")) +
                                       source.filename().string());
    const uint64_t task = m_importTask;
    const fs::path dest = host.AssetsCwd();
    m_import = std::async(std::launch::async, [source, dest, task]() {
        return CopyIntoProject(source, dest, [task](float fraction, const std::string& what) {
            sage::editor::progress::Update(task, fraction, what);
        });
    });
}


// Принять то, что скопировал фоновый поток: зарегистрировать в базе ассетов и
// сказать человеку, чем кончилось. Только главный поток — см. AssetsPanel.h.
void AssetsPanel::FinishImport(EditorHost& host) {
    namespace progress = sage::editor::progress;
    if (!m_import.valid()) return;
    if (m_import.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const ImportReport r = m_import.get();
    m_import = {};

    if (!r.Ok) {
        progress::End(m_importTask, T("Import failed: ") + r.Error);
        m_importTask = 0;
        host.SetStatusMessage(T("Import failed: ") + r.Error);
        LOG_ERROR("Editor") << "Импорт не удался: " << r.Error;
        return;
    }
    RegisterImported(r);
    m_selected = r.Created;
    m_multi = {r.Created};
    // Модели среди внесённого — сразу окно настроек импорта:
    // ось, масштаб, листва задаются один раз на ассет, до того как модель
    // встанет в сцену неправильно.
    sage::editor::modelimport::Ask(sage::editor::modelimport::ModelsNeedingSettings(r.Created));

    std::string message = T("Brought in: ") + r.Created.filename().string();
    if (!r.Extra.empty()) message += " (+" + std::to_string(r.Extra.size()) + T(" file(s) inside)");
    if (!r.Missing.empty()) {
        // Недостающие спутники (и пропущенные записи архива) — это будущее
        // «модель без текстуры», и узнать о них надо здесь. Список уходит в
        // консоль целиком: в статусной строке ему не поместиться, а первое имя
        // уже подсказывает, что искать.
        message += T("; not brought in: ") + r.Missing.front();
        if (r.Missing.size() > 1) message += T(" and ") + std::to_string(r.Missing.size() - 1);
        for (const std::string& m : r.Missing)
            LOG_WARN("Editor") << "Импорт: не внесено — " << m;
    }
    progress::End(m_importTask, message);
    m_importTask = 0;
    host.SetStatusMessage(message);
    LOG_INFO("Editor") << "Импорт: " << r.Created.string();
}

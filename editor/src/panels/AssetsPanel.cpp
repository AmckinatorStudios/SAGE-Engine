#include "../AssetOpen.h"
#include "sage/core/Paths.h"
#include "../CreateAssetDialog.h"
#include "../DragSet.h"
#include "../PanelWindowId.h"
#include "../PanelWindows.h"
#include "AssetsPanel.h"
#include "../ui/TreeLines.h"
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

namespace fs = std::filesystem;


namespace covers = sage::editor::covers;

#include "AssetsPanelDetail.h"
using namespace sage::editor::assetspanel_detail;


// КОРЕНЬ ПАНЕЛИ — assets/ ПРОЕКТА, и выше него панель не поднимается.
//
// Всё, с чем панель умеет работать, живёт в assets/: только оттуда ассет
// попадает в сцену ссылкой, которая переживёт сборку игры, и только там его
// найдут импорт, превью и слоты. Выше лежат служебные файлы проекта
// (project.sageproj, sage.cfg, папка scenes/) — трогать их отсюда нечем, а
// показывать значит предлагать работу, которой панель не делает: человек
// заходил в папку проекта, видел «пусто» и решал, что ассеты потерялись.
//
// Возврат пути наружу тоже закрыт этим: путь за пределами assets/ в сцене не
// работает (Project::AssetRef), и запрещать его в слотах, продолжая водить туда
// панелью, — это два правила об одном и том же, из которых верно только одно.
fs::path AssetsPanel::AssetsRoot(EditorHost& host) {
    Project& project = host.CurrentProject();
    return project.Loaded() ? project.AssetsDir() : host.AssetsCwd();
}

// Загоняет текущую папку обратно в корень, если она оказалась снаружи. Так
// бывает после открытия другого проекта и после удаления папки, в которой
// стояли: молча остаться «нигде» хуже, чем вернуться в корень.
void AssetsPanel::ClampCwd(EditorHost& host) {
    namespace fscache = sage::editor::fscache;
    const fs::path root = AssetsRoot(host);
    fs::path& cwd = host.AssetsCwd();
    // «assets/Models/» и «assets/Models» — одна папка, но у первого пустое
    // имя: путь показал бы лишний сегмент, а сравнения с деревом не сошлись бы.
    while (!cwd.has_filename() && cwd.has_parent_path() && cwd.parent_path() != cwd) cwd = cwd.parent_path();
    // Папки нет (удалили — здесь или проводником) — поднимаемся к БЛИЖАЙШЕЙ
    // уцелевшей, а не в корень: удалили «Characters» — остаёмся в «Models»,
    // откуда и смотрели. С памятью (FsCache): вопрос задаётся каждый кадр.
    while (fscache::Stamp(cwd, 0.5) == 0 && cwd.has_parent_path() && cwd.parent_path() != cwd)
        cwd = cwd.parent_path();
    const fs::path& c = fscache::Canonical(cwd);
    const fs::path& r = fscache::Canonical(root);
    for (fs::path p = c; ; p = p.parent_path()) {
        if (p == r) return;                  // внутри корня — всё в порядке
        if (!p.has_parent_path() || p.parent_path() == p) break;
    }
    cwd = root;
}

// Открыть файл — по одному правилу для двойного щелчка и пункта меню
// (AssetOpen.h).
void AssetsPanel::OpenItem(EditorHost& host, const fs::path& path, bool isDir) {
    using sage::editor::assetopen::OpenAs;
    switch (sage::editor::assetopen::Of(path, isDir)) {
        case OpenAs::Folder: host.AssetsCwd() = path; break;
        case OpenAs::Scene: host.LoadSceneFromFile(path); break;
        case OpenAs::Prefab: host.InstantiatePrefab(path); break;   // инстанс в сцену
        // Клип открывается инструментом анимации — и свой, и вынутый из модели:
        // второй тоже надо уметь посмотреть и проиграть, просто править его
        // нельзя (см. panels/AnimationPanel.h).
        case OpenAs::Clip: host.OpenAnimationClip(path.string()); break;
        case OpenAs::AiGraph: host.OpenAiGraph(sage::PathToUtf8(path)); break;
        case OpenAs::AnimGraph: host.OpenAnimGraph(sage::PathToUtf8(path)); break;
        case OpenAs::VisualScript: host.OpenVisualScript(sage::PathToUtf8(path)); break;
        // Интерфейс правится в своём пространстве, а не разворачивается в сцену.
        case OpenAs::Interface: host.OpenInterface(host.CurrentProject().AssetRef(path)); break;
        case OpenAs::Text:
            // Текст открывается ТЕМ, ЧЕМ ЕГО ОТКРЫВАЕТ СИСТЕМА (или выбранным
            // редактором кода). Отказ виден: молчащий щелчок неотличим от
            // сломанного редактора.
            if (!host.OpenFileInSystemEditor(path))
                host.SetStatusMessage(T("The system has no program for this file: ") + path.filename().string());
            break;
        case OpenAs::None: break;
    }
}

// Конвертация одного файла. Отчёт уходит в статусную строку: человек нажал
// пункт меню и обязан увидеть, что получилось, не открывая консоль.
void AssetsPanel::ConvertOne(EditorHost& host, const fs::path& path) {
    sage::assets::ConvertOptions opts;
    opts.Overwrite = true;   // явное действие по одному файлу — перезапись ожидаема
    const sage::assets::ConvertResult r = sage::assets::ConvertAnyToNative(path.string(), {}, opts);
    if (!r.Ok) {
        host.SetStatusMessage(T("Conversion failed: ") + r.Error);
        return;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), T("%s -> %s (%.1fx smaller)"),
                  path.filename().string().c_str(),
                  fs::path(r.OutputPath).filename().string().c_str(), (double)r.Ratio());
    host.SetStatusMessage(buf);
    for (const std::string& w : r.Warnings) host.SetStatusMessage(T("Import: ") + w);
}

// Вся текущая папка. Отчёт — одной строкой со сводкой: перечислять полсотни
// файлов в статусной строке бессмысленно, а подробности уже в консоли.
// ПАПКА ПЕРЕВОДИТСЯ ПО КАДРАМ, А НЕ ОДНИМ ВЫЗОВОМ.
//
// Раньше здесь стоял sage::assets::ConvertFolder: он перемалывал всю папку
// внутри одного кадра. Сотня моделей — это минуты, в которые редактор не
// рисовал ничего: ни полосы, ни имени файла, ни даже курсора. Со стороны это
// неотличимо от зависания, и первое, что делает человек, — снимает задачу,
// теряя вместе с ней уже сделанную работу.
//
// Поэтому здесь только СПИСОК: что переводить. Сами файлы разбирает Tick — по
// нескольку за кадр, под модальным окном прогресса (см. Progress.h). Модальным
// именно потому, что папка проекта в это время меняется под ногами.
void AssetsPanel::ConvertFolderHere(EditorHost& host) {
    if (!m_convertQueue.empty()) return;   // одна пакетная работа за раз

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = host.AssetsCwd();
    m_convertQueue.clear();
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string path = entry.path().string();
        if (!sage::assets::IsConvertibleModel(path) && !sage::assets::IsConvertibleTexture(path))
            continue;
        m_convertQueue.push_back(entry.path());
    }
    if (m_convertQueue.empty()) {
        host.SetStatusMessage(T("Nothing to convert: no models or images in the folder"));
        return;
    }
    m_convertAt = 0;
    m_convertOk = m_convertFailed = 0;
    m_convertSrcBytes = m_convertOutBytes = 0;
    namespace progress = sage::editor::progress;
    m_convertTask = progress::Begin(progress::Kind::Blocking, T("Converting the folder"));
}

void AssetsPanel::Tick(EditorHost& host) {
    FinishImport(host);
    if (m_convertQueue.empty()) return;
    namespace progress = sage::editor::progress;

    // Сколько файлов за кадр. Один — и папка из тысячи мелких текстур
    // переводилась бы минуту на ровном месте; много — и кадр снова встаёт.
    // Четыре держат и то, и другое: кадр остаётся живым, полоса едет.
    constexpr size_t kPerFrame = 4;
    sage::assets::ConvertOptions opts;
    opts.Overwrite = false;   // пакетная операция не должна затирать правки руками

    for (size_t done = 0; done < kPerFrame && m_convertAt < m_convertQueue.size(); ++done) {
        const std::filesystem::path& file = m_convertQueue[m_convertAt++];
        const sage::assets::ConvertResult r =
            sage::assets::ConvertAnyToNative(file.string(), {}, opts);
        if (r.Ok) {
            ++m_convertOk;
            m_convertSrcBytes += r.SourceBytes;
            m_convertOutBytes += r.OutputBytes;
        } else {
            ++m_convertFailed;
            LOG_WARN("Convert") << r.SourcePath << ": " << r.Error;
        }
    }

    const size_t total = m_convertQueue.size();
    char note[256];
    if (m_convertAt < total) {
        std::snprintf(note, sizeof(note), "%s (%zu/%zu)",
                      m_convertQueue[m_convertAt].filename().string().c_str(), m_convertAt, total);
        progress::Update(m_convertTask, (float)m_convertAt / (float)total, note);
        return;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf), T("Converted %zu, skipped %zu; %.1f -> %.1f KB"), m_convertOk,
                  m_convertFailed, m_convertSrcBytes / 1024.0, m_convertOutBytes / 1024.0);
    progress::End(m_convertTask, buf);
    m_convertTask = 0;
    m_convertQueue.clear();
    m_convertAt = 0;
    host.SetStatusMessage(buf);
}

// ============================================================================
//  Внесение чужих файлов в проект
// ============================================================================

// Модалки панели. Деферред-паттерн: контекстные меню только выставляют
// m_renameTarget/m_deleteTarget, OpenPopup зовётся здесь, на
// уровне окна панели (OpenPopup изнутри BeginMenu не находит модалку —
// другой ID-стек, документированная ловушка ImGui).
void AssetsPanel::DrawModals(EditorHost& host) {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();

    if (!m_renameTarget.empty() && !ImGui::IsPopupOpen("Rename Asset")) {
        ImGui::OpenPopup("Rename Asset");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Rename Asset" "###Rename Asset"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", m_renameTarget.filename().string().c_str());
        if (ImGui::IsWindowAppearing()) {
            std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s",
                          m_renameTarget.filename().string().c_str());
        }
        bool enterPressed = ImGui::InputText(T("New name"), m_renameBuf, sizeof(m_renameBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (enterPressed || ImGui::Button(T("Rename"), ImVec2(120, 0))) {
            if (ApplyRename(host, m_renameTarget, m_renameBuf, m_error)) {
                m_renameTarget.clear();
                m_error.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) {
            m_renameTarget.clear();
            m_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ВОПРОС ЗАДАЁТСЯ ОДИН РАЗ. Раньше окно открывалось каждый кадр, пока
    // цели не пусты, — то есть закрыть его Escape'ом было нельзя: следующий же
    // кадр открывал его заново. Окно, от которого нельзя отказаться, читается
    // как зависший редактор.
    if (!m_deleteTargets.empty() && !m_deleteAsked) {
        ImGui::OpenPopup("Delete Asset");
        m_deleteAsked = true;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Delete Asset" "###Delete Asset"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Один файл называется по имени, набор — числом. «Удалить 17 файлов?»
        // отвечает на тот же вопрос, что и имя: понимаю ли я, что сейчас
        // исчезнет.
        if (m_deleteTargets.size() == 1) {
            ImGui::Text(T("Delete \"%s\"?"), m_deleteTargets.front().filename().string().c_str());
        } else {
            ImGui::Text(T("Delete selected files: %zu?"), m_deleteTargets.size());
        }
        ImGui::TextDisabled("%s", T("This cannot be undone."));
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::Danger));
        if (ImGui::Button(T("Delete"), ImVec2(120, 0))) {
            ApplyDelete(host, m_deleteTargets);
            m_deleteTargets.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        // ESCAPE — ТА ЖЕ «ОТМЕНА». Само ImGui модальное окно по Escape не
        // закрывает (это сделано нарочно: модалка требует ответа), но вопрос
        // «удалить?» отменяют именно так, и без этого выход из него был только
        // один — попасть мышью в кнопку.
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_deleteTargets.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (m_deleteAsked && !ImGui::IsPopupOpen("Delete Asset")) {
        // Окно закрыли, ничего не выбрав (Escape или щелчок мимо) — значит
        // передумали, и цели удаления больше не ждут ответа.
        m_deleteTargets.clear();
    }
    if (m_deleteTargets.empty()) m_deleteAsked = false;
}

// Бросок строки из иерархии на панель Assets: объект становится префабом в
// папке folder. Подсказка у курсора — ещё до отпускания, как у слотов.
void sage::editor::assetspanel_detail::AcceptObjectAsPrefab(EditorHost& host, const fs::path& folder) {
    const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ENTITY", ImGuiDragDropFlags_AcceptBeforeDelivery);
    if (!p || p->DataSize != (int)sizeof(int)) return;
    // Группа объектов — по префабу на каждый: так раскладывают набор
    // заготовок (двери, лампы, враги) одним броском.
    const std::vector<int> group = dragset::Entities(p);
    ImGui::SetTooltip("%s", group.size() > 1 ? T("Drop to save each object as a prefab here")
                                             : T("Drop to save the object as a prefab here"));
    if (!p->IsDelivery()) return;
    for (int id : group) {
        std::string err;
        const fs::path made = host.SaveObjectAsPrefab(id, folder, err);
        if (made.empty()) host.SetStatusMessage(T("The prefab was not saved: ") + err);
    }
}

void AssetsPanel::Draw(EditorHost& host, bool* open, const std::string& windowId) {
    if (!m_prefsLoaded) LoadPrefs();
    // Бюджет превью на кадр: см. ThumbnailFor.
    m_thumbRenderedThisFrame = false;
    Project& project = host.CurrentProject();
    ClampCwd(host);

    // Просьба вывести вкладку вперёд: панель делит место с консолью, и пока
    // впереди консоль, панели ассетов в кадре нет вовсе (см. RequestFocus).
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::Begin(sage::editor::panelid::Title("folder-full", T("Assets"), windowId).c_str(), open,
                     panelwindows::WindowFlags("Assets"));

    DrawHeader(host);

    // Сломанные ссылки — В ПАНЕЛИ, а не только в логе: сцена грузилась, объект
    // стоял без модели, и заметить это можно было лишь случайно.
    const auto& broken = sage::AssetDatabase::Instance().Broken();
    if (!broken.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Danger));
        const bool open = ImGui::CollapsingHeader(
            (T("Broken references: ") + std::to_string(broken.size()) + "###brokenrefs").c_str());
        ImGui::PopStyleColor();
        if (open) {
            for (const auto& b : broken) {
                ImGui::BulletText("%s", b.Hint.empty() ? b.Missing.ToString().c_str()
                                                       : b.Hint.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "%s", T("The file is missing. Put it back or reassign the asset —\n"
                          "only the .meta sidecar could have caught a rename made outside the editor."));
                }
            }
            if (ImGui::Button(T("Rescan project"))) {
                sage::AssetDatabase::Instance().ClearBroken();
                sage::AssetDatabase::Instance().ScanProject(project.Dir().string());
            }
        }
    }

    // Какой проект обслуживаем (метки папок лежат в нём). Дёшево: перечитывает
    // файл только при СМЕНЕ проекта.
    sage::editor::foldercolors::SetProject(host.CurrentProject().Dir());

    // Дерево — ОДНО на оба вида: переключатель меняет только правую область.
    DrawFolderTree(host);

    // С Ctrl колесо — масштабу (ниже), и прокрутка в этот момент не нужна.
    ImGui::BeginChild("##assets_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGui::GetIO().KeyCtrl ? ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None);
    namespace rectselect = sage::editor::rectselect;
    m_rectActive = rectselect::Begin(m_rect);
    m_rectHits.clear();
    m_tileCenters.clear();
    RefreshItems(host);

    // Ctrl + колесо — масштаб содержимого, как в проводнике: смотрят на сетку,
    // а не на ползунок в шапке.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().KeyCtrl &&
        ImGui::GetIO().MouseWheel != 0.0f) {
        const float step = ImGui::GetIO().MouseWheel > 0.0f ? 1.0f : -1.0f;
        if (m_view == ViewMode::Grid) SetTileSize(m_tileSize + step * 12.0f);
        else SetRowHeight(m_rowHeight + step * 4.0f);
    }

    if (m_view == ViewMode::Grid) DrawGrid(host);
    else DrawList(host);

    if (m_items.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", m_search[0] ? T("Nothing matches the search.")
                                             : T("This folder is empty."));
        ImGui::TextDisabled("%s", T("Right-click to create a folder, script, material or text file."));
    }

    // Создание ассетов — ПКМ по пустому месту (у элементов своё меню).
    // MenuScope — в границах if: деструктор снимается до EndChild()/End().
    if (Sage::UI::MenuScope createCtxMenu; ImGui::BeginPopupContextWindow("##assets_create_ctx",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        DrawCreateMenu(host);
        ImGui::EndPopup();
    }

    // DELETE — КЛАВИШЕЙ, А НЕ ТОЛЬКО ЧЕРЕЗ МЕНЮ. Выделил сорок файлов рамкой,
    // потянулся к Delete — и ничего: удалить можно было только правой кнопкой
    // по одной из карточек, то есть жестом, который ещё надо вспомнить.
    // Условия жёсткие намеренно: клавиша срабатывает, только когда работают
    // ИМЕННО С ЭТОЙ панелью и ничего не печатают, — иначе Delete в поле поиска
    // или в переименовании сносил бы файлы вместо буквы.
    //
    // ФОКУС СПРАШИВАЕТСЯ У ВСЕЙ ПАНЕЛИ (RootAndChildWindows), а не у текущего
    // окна с его детьми. Эта проверка стоит ВНУТРИ дочернего окна прокрутки, и
    // ChildWindows означал здесь «фокус на самой прокрутке или ниже» — то есть
    // клавиша работала, только если последним щёлкнули по сетке карточек. Щелчок
    // по строке пути, по полю фильтра или по дереву папок слева фокусировал
    // другое дочернее окно той же панели, и Delete переставал работать, ничего
    // об этом не сообщая. Панель тут одна, и вопрос к ней тоже один.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput) {
        // Delete принадлежит панели ассетов, пока она в фокусе: иначе он
        // доходил бы до общего обработчика и удалял объект из СЦЕНЫ — человек
        // смотрел в дерево файлов, а лишался объекта.
        sage::editor::hotkeys::Claim(sage::editor::hotkeys::Key::Delete);
    }
    if (!m_multi.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput && m_renameTarget.empty() &&
        !sage::editor::createasset::IsOpen() && m_deleteTargets.empty() &&
        !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        // Только ВИДИМОЕ выделенное: файл из прошлой папки или скрытый
        // поиском клавишей не удаляется — человек его не видит.
        m_deleteTargets = DeletableSelection();
    }
    // Ctrl+A — всё, что видно в папке (с учётом поиска).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsAnyItemActive() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false) &&
        !m_visibleOrder.empty()) {
        m_multi = m_visibleOrder;
        m_selected = m_visibleOrder.back();
        m_anchor = m_visibleOrder.front();
    }

    // Рамка выделения. Начинается только в пустом месте сетки: над карточкой
    // живёт перетаскивание файла, и рамка отняла бы его.
    // Отложенный одиночный выбор: потянули — набор уехал целиком; отпустили
    // на месте — выбран один файл.
    if (!m_pendingPick.empty()) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            m_pendingPick.clear();
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            m_selected = m_pendingPick;
            m_multi = {m_pendingPick};
            m_anchor = m_pendingPick;
            m_pendingPick.clear();
        }
    }

    if (m_rectActive && m_rect.Finished) {
        if (rectselect::Meaningful(m_rect)) {
            if (!m_rect.Additive) m_multi.clear();
            for (const fs::path& hit : m_rectHits) {
                if (std::find(m_multi.begin(), m_multi.end(), hit) == m_multi.end())
                    m_multi.push_back(hit);
            }
            m_selected = m_multi.empty() ? fs::path{} : m_multi.back();
        } else if (!m_rect.Additive) {
            // Щелчок по пустому месту сетки снимает выбор файла — тем же
            // жестом, что и в списке объектов и во вьюпорте.
            m_multi.clear();
            m_selected.clear();
        }
    }
    rectselect::End(m_rect, ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                                !ImGui::IsAnyItemHovered() &&
                                !ImGui::IsPopupOpen("##assets_create_ctx"));
    rectselect::Draw(m_rect);

    // ОБЪЕКТ ИЗ ИЕРАРХИИ — В ПАНЕЛЬ: новый префаб в открытой папке. Цель — всё
    // окно сетки, а не отдельная карточка: бросают «сюда, в эту папку», и
    // промахнуться мимо пустого места нельзя.
    {
        const ImVec2 wmin = ImGui::GetWindowPos();
        const ImVec2 wmax(wmin.x + ImGui::GetWindowSize().x, wmin.y + ImGui::GetWindowSize().y);
        if (ImGui::BeginDragDropTargetCustom(ImRect(wmin, wmax), ImGui::GetID("##assets_object_drop"))) {
            AcceptObjectAsPrefab(host, host.AssetsCwd());
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::EndChild();

    DrawModals(host);

    ImGui::End();
}

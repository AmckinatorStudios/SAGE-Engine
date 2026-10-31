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
    const fs::path root = AssetsRoot(host);
    fs::path& cwd = host.AssetsCwd();
    std::error_code ec;
    if (!fs::exists(cwd, ec)) { cwd = root; return; }
    const fs::path c = fs::weakly_canonical(cwd, ec);
    const fs::path r = fs::weakly_canonical(root, ec);
    for (fs::path p = c; ; p = p.parent_path()) {
        if (p == r) return;                  // внутри корня — всё в порядке
        if (!p.has_parent_path() || p.parent_path() == p) break;
    }
    cwd = root;
}

void AssetsPanel::DrawBreadcrumb(EditorHost& host) {
    fs::path& cwd = host.AssetsCwd();
    // Цепочка обрывается на КОРНЕ ПАНЕЛИ: показывать дорогу туда, куда всё
    // равно не пустят, — значит обещать несуществующий ход.
    const fs::path root = AssetsRoot(host);

    std::vector<fs::path> chain;
    std::error_code cec;
    const fs::path& rootCanon = sage::editor::fscache::Canonical(root);
    for (fs::path p = cwd; p.has_parent_path(); p = p.parent_path()) {
        chain.push_back(p);
        if (sage::editor::fscache::Canonical(p) == rootCanon) break;
        if (p.parent_path() == p) break; // достигли корня файловой системы
    }
    std::reverse(chain.begin(), chain.end());

    // Хвост, а не вся цепочка, и хвост ПО ШИРИНЕ ОКНА. Без проекта cwd — это
    // абсолютный путь вроде /home/user/work/<длинный-uuid>/assets: полная
    // цепочка съедала строку целиком и выталкивала за край окна всё, что стояло
    // правее. Начало пути всё равно никому не нужно — нужно то место, где
    // стоишь, и дорога на пару шагов назад. Сколько шагов поместится, столько и
    // показываем: панель Assets сужают до трети экрана, и фиксированное число
    // сегментов при такой ширине снова полезло бы за край.
    const float avail = ImGui::GetContentRegionAvail().x - 24.0f; // запас под «…/»
    const float sepW = ImGui::CalcTextSize(" / ").x;
    float used = 0.0f;
    size_t first = chain.size();
    while (first > 0) {
        std::string seg = chain[first - 1].filename().string();
        if (seg.empty()) seg = chain[first - 1].string();
        const float w = ImGui::CalcTextSize(seg.c_str()).x + 8.0f + sepW;
        if (first < chain.size() && used + w > avail) break;
        used += w;
        --first;
    }

    // Кнопки хлебных крошек — без рамки: это путь, а не ряд кнопок. Рамка
    // появляется под курсором, чтобы было видно, что по сегменту можно кликнуть.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, ImGui::GetStyle().FramePadding.y));
    if (first > 0) {
        ImGui::TextDisabled("...");
        ImGui::SameLine(0, 2);
        ImGui::TextDisabled("/");
        ImGui::SameLine(0, 2);
    }
    for (size_t i = first; i < chain.size(); ++i) {
        std::string seg = chain[i].filename().string();
        if (seg.empty()) seg = chain[i].string();
        ImGui::PushID(static_cast<int>(i));
        // Текущая папка — не кнопка: щёлкать по ней некуда, и подсветка при
        // наведении обещала бы переход, которого не будет.
        if (i + 1 == chain.size()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(seg.c_str());
        } else if (ImGui::Button(seg.c_str())) {
            cwd = chain[i];
        }
        ImGui::PopID();
        if (i + 1 < chain.size()) { ImGui::SameLine(0, 2); ImGui::TextDisabled("/"); ImGui::SameLine(0, 2); }
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}


void AssetsPanel::DrawFolderNode(EditorHost& host, const fs::path& dir, int depth) {
    // Подпапки и «та ли это папка» — с памятью (FsCache.h): дерево рисуется
    // каждый кадр, и листать диск на каждую его строку в каждом кадре — это и
    // был заметный кусок просевшего FPS на наборе с сотней папок.
    namespace fscache = sage::editor::fscache;
    const std::vector<fs::path>& subdirs = fscache::List(dir, 0.5).Dirs;

    fs::path& cwd = host.AssetsCwd();
    const bool current = fscache::Canonical(cwd) == fscache::Canonical(dir);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DrawLinesNone;
    if (subdirs.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (current) flags |= ImGuiTreeNodeFlags_Selected;
    // Корень раскрыт сразу: свёрнутое дерево из одной строки не отвечает ни на
    // один вопрос и требует лишнего щелчка каждый раз.
    if (depth == 0) ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);

    ImGui::PushID(dir.string().c_str());
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const float indent = ImGui::GetTreeNodeToLabelSpacing();
    // Строка — в общие линии дерева (ui/TreeLines.h): у папки с подпапками
    // горизонталь от родителя кончается ДО стрелки, а не перечёркивает её.
    m_treeLines.Row(rowPos.y, depth, rowPos.x + ImGui::GetStyle().FramePadding.x, rowPos.x + indent,
                    !subdirs.empty());
    const bool open = ImGui::TreeNodeEx("##folder", flags, "%s", "");
    // Значок и цвет — ТЕ ЖЕ, что в сетке, и из того же источника.
    //
    // Здесь стояло `subdirs.empty() ? "folder" : "folder-full"` — то есть
    // заполненность считалась по ОДНИМ ПОДПАПКАМ. Папка с полусотней текстур и
    // без единой подпапки выглядела в дереве пустой, а в сетке — полной: два
    // места отвечали на один вопрос по-разному, и верным было второе.
    // FolderIcon (см. ниже) знает про содержимое целиком и про служебные
    // сайдкары .meta, которые содержимым не считаются.
    //
    // Значок и подпись — одной парой с общим зазором (EditorIcons::DrawLabeled),
    // а не двумя пробелами в формате.
    {
        glm::vec3 tint(0.85f, 0.68f, 0.32f);
        sage::editor::foldercolors::Get(dir, tint);
        const ImVec4 c(tint.x, tint.y, tint.z, 1.0f);
        EditorIcons::DrawLabeled(ImGui::GetWindowDrawList(), ImVec2(rowPos.x + indent, rowPos.y),
                                 ImGui::GetTextLineHeight(), FolderIcon(dir), ImGui::GetColorU32(c),
                                 dir.filename().string().c_str(),
                                 ImGui::GetColorU32(ImGuiCol_Text));
    }
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) cwd = dir;

    // ЦВЕТНАЯ МЕТКА СТАВИТСЯ И ОТСЮДА. Меню было только у карточки в сетке:
    // метку видно в обоих местах, а назначить её можно было ровно в одном —
    // причём не в том, где папки и ищут. Подменю общее (см. FolderColors.h),
    // поэтому палитра в дереве и в сетке не разъедется.
    if (Sage::UI::MenuScope folderMenu; ImGui::BeginPopupContextItem("##folder_menu")) {
        ImGui::TextDisabled("%s", dir.filename().string().c_str());
        ImGui::Separator();
        sage::editor::foldercolors::DrawPaletteMenu(dir);
        if (EditorIcons::MenuItem("folder", T("Show in Assets"))) cwd = dir;
        ImGui::EndPopup();
    }
    // Бросок файла на папку дерева — перенос в неё: то же, что и в сетке.
    // Бросок ОБЪЕКТА из иерархии — новый префаб в этой папке.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH"))
            MoveGroupIntoFolder(host, dragset::Assets(p), dir);
        AcceptObjectAsPrefab(host, dir);
        ImGui::EndDragDropTarget();
    }
    if (open && !subdirs.empty()) {
        for (const fs::path& sub : subdirs) DrawFolderNode(host, sub, depth + 1);
        // Линии связи — те же, что в иерархии (ui/TreeLines.h): встроенная в
        // ImGui горизонталь обрывается далеко от значка папки, а своя копия здесь
        // вела линию сквозь стрелку раскрытия.
        m_treeLines.Draw(rowPos, indent, depth + 1);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void AssetsPanel::DrawFolderTree(EditorHost& host) {
    ImGui::BeginChild("##assets_tree", ImVec2(m_treeWidth, 0), ImGuiChildFlags_Borders);
    m_treeLines.Clear();
    // Корень дерева — assets/, а не папка проекта: см. AssetsRoot().
    DrawFolderNode(host, AssetsRoot(host), 0);
    ImGui::EndChild();

    // Разделитель: ширину дерева правят перетаскиванием. Не настройка в меню —
    // её меняют на глаз, пока смотрят на имена, которые не помещаются.
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("##assets_split", ImVec2(6.0f, ImGui::GetContentRegionAvail().y));
    if (ImGui::IsItemActive()) m_treeWidth += ImGui::GetIO().MouseDelta.x;
    m_treeWidth = std::clamp(m_treeWidth, 120.0f, 420.0f);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2((a.x + b.x) * 0.5f - 1.0f, a.y), ImVec2((a.x + b.x) * 0.5f + 1.0f, b.y),
            ImGui::GetColorU32(ImGuiCol_NavHighlight));
    }
    ImGui::SameLine(0.0f, 0.0f);
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
            fs::path target;
            if (!RenameAsset(m_renameTarget, m_renameBuf, target, m_error)) {
                LOG_ERROR("Editor") << "Asset rename failed: " << m_error;
            } else {
                sage::editor::foldercolors::Rename(m_renameTarget, target);
                if (m_selected == m_renameTarget) m_selected = target;
                for (fs::path& p : m_multi) if (p == m_renameTarget) p = target;
                sage::AssetDatabase::Instance().ScanProject(
                    host.CurrentProject().Dir().string());
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
            for (const fs::path& victim : m_deleteTargets) {
                DeleteAsset(victim);
                if (m_selected == victim) m_selected.clear();
                auto it = std::find(m_multi.begin(), m_multi.end(), victim);
                if (it != m_multi.end()) m_multi.erase(it);
            }
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
    // Бюджет превью на кадр: см. ThumbnailFor.
    m_thumbRenderedThisFrame = false;
    Project& project = host.CurrentProject();
    ClampCwd(host);
    fs::path& cwd = host.AssetsCwd();

    // Просьба вывести вкладку вперёд: панель делит место с консолью, и пока
    // впереди консоль, панели ассетов в кадре нет вовсе (см. RequestFocus).
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::Begin(sage::editor::panelid::Title("folder-full", T("Assets"), windowId).c_str(), open,
                     panelwindows::WindowFlags("Assets"));

    // --- Шапка панели: две строки с ЯСНЫМ разделением обязанностей ---
    //
    // Раньше здесь в одну строку выкладывались SmallButton «Вверх», хлебные
    // крошки, обычная кнопка «Импорт…» и следом поле поиска во всю ширину.
    // Виджеты разной высоты в одном ряду и строка поиска, ничем не отделённая
    // от содержимого, и читались как «интерфейс кривой и смешанный»: глазу не
    // за что зацепиться, потому что группы не выделены. Теперь первая строка —
    // ТОЛЬКО навигация (где я нахожусь), вторая — ТОЛЬКО действия над этой
    // папкой (что я здесь ищу и что приношу), и все виджеты в ряду одной высоты.
    // Выше корня проекта панель не поднимается: снаружи проекта её файлы
    // редактору не принадлежат, а ссылка на них не переживёт сборку игры.
    const fs::path root = AssetsRoot(host);
    std::error_code upec;
    const bool atRoot = sage::editor::fscache::Canonical(cwd) ==
                        sage::editor::fscache::Canonical(root);
    (void)upec;
    const bool canGoUp = cwd.has_parent_path() && !atRoot;
    ImGui::BeginDisabled(!canGoUp);
    if (EditorIcons::IconOnlyButton("up", T("Up"))) cwd = cwd.parent_path();
    ImGui::EndDisabled();
    ImGui::SameLine(0, 6);
    DrawBreadcrumb(host);

    // Внесение файлов со стороны. До сих пор этого не было вовсе: панель умела
    // ходить по папкам проекта и создавать пустые ассеты, а положить в проект
    // СВОЮ модель или картинку можно было только мимо редактора — проводником.
    // Человек при этом обычно шёл другим путём: выбирал файл прямо из «Загрузок»
    // через «Обзор…», получал в сцене абсолютный путь и рабочий вид ровно до
    // первой сборки игры (см. Project::AssetRef).
    const float importW = ImGui::CalcTextSize(T("Import...")).x +
                          ImGui::GetFrameHeight() * 0.68f + ImGui::GetStyle().FramePadding.x * 3.0f;
    // Через Sage::UI: одно поле поиска на весь редактор — с иконкой внутри,
    // тем же отступом и той же высотой, что в консоли и в палитре команд. Три
    // самодельных поля выглядели тремя разными полями.
    const float searchW = std::max(120.0f, ImGui::GetContentRegionAvail().x - importW -
                                               ImGui::GetStyle().ItemSpacing.x);
    Sage::UI::SearchField("assets_search", m_search, sizeof(m_search), T("Search..."), searchW);
    ImGui::SameLine();
    DrawImportButton(host);

    // Сломанные ссылки — В ПАНЕЛИ, а не только в логе. Именно молчание и было
    // исходной болезнью: сцена грузилась, объект стоял на месте, просто без
    // модели, и заметить это можно было лишь случайно.
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
    ImGui::Separator();

    // Какой проект обслуживаем (метки папок лежат в нём). Дёшево: перечитывает
    // файл только при СМЕНЕ проекта.
    sage::editor::foldercolors::SetProject(host.CurrentProject().Dir());

    DrawFolderTree(host);

    ImGui::BeginChild("##assets_scroll");
    namespace rectselect = sage::editor::rectselect;
    m_rectActive = rectselect::Begin(m_rect);
    m_rectHits.clear();
    std::error_code ec;
    // Содержимое папки — из кэша листингов (FsCache.h): перечитывается, когда
    // меняется время правки самой папки (добавили, удалили, переименовали), а
    // не в каждом кадре. Сайдкары .meta кэш не показывает: это служебная
    // запись движка (GUID ассета, см. AssetDatabase) — по одной НА КАЖДЫЙ
    // файл, и папка с двадцатью ассетами показывала бы сорок карточек.
    const sage::editor::fscache::Listing& listing = sage::editor::fscache::List(cwd, 0.0);
    const std::vector<fs::path>& dirs = listing.Dirs;
    const std::vector<fs::path>& files = listing.Files;

    std::string filter = ToLower(m_search);
    auto matches = [&](const fs::path& p) {
        if (filter.empty()) return true;
        return ToLower(p.filename().string()).find(filter) != std::string::npos;
    };

    // Единый ритм по вертикали между строками грида — как горизонтальный зазор.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kTileSpacing, kTileSpacing));
    float availWidth = ImGui::GetContentRegionAvail().x;
    int columns = std::max(1, static_cast<int>((availWidth + kTileSpacing) / (kTileW + kTileSpacing)));
    int col = 0;
    bool any = false;
    // Порядок карточек НА ЭКРАНЕ собирается ДО отрисовки: по нему считается
    // диапазон Shift-выбора, а щелчок разбирается внутри DrawTile — то есть
    // раньше, чем нарисуются карточки ниже (см. m_visibleOrder).
    m_visibleOrder.clear();
    for (const auto& d : dirs) if (matches(d)) m_visibleOrder.push_back(d);
    for (const auto& f : files) if (matches(f)) m_visibleOrder.push_back(f);

    m_tileCenters.clear();
    auto placeTile = [&](const fs::path& p, bool isDir) {
        any = true;
        if (col > 0) ImGui::SameLine(0, kTileSpacing);
        DrawTile(host, p, isDir);
        // Где карточка на экране: самопроверка щёлкает по ней по-настоящему
        // (см. EditorLayer::TickInputProbe).
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        m_tileCenters.push_back(ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f));
        col = (col + 1) % columns;
    };
    for (const auto& d : dirs) if (matches(d)) placeTile(d, true);
    for (const auto& f : files) if (matches(f)) placeTile(f, false);
    ImGui::PopStyleVar();

    // Обложки файлов, которых в этой папке нет, больше не нужны — отпускаем и
    // их буферы. Иначе за сеанс блуждания по проекту накопился бы буфер на
    // КАЖДЫЙ ассет, который когда-либо показали, и все они висели бы в
    // видеопамяти до выхода из редактора.
    for (auto it = m_thumbs.begin(); it != m_thumbs.end();) {
        std::error_code exists;
        if (fs::path(it->first).parent_path() == cwd && fs::exists(it->first, exists)) {
            ++it;
            continue;
        }
        m_preview.ReleaseTarget(it->first);
        it = m_thumbs.erase(it);
    }
    if (!any) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", m_search[0] ? T("Nothing matches the search.")
                                             : T("This folder is empty."));
        ImGui::TextDisabled("%s", T("Right-click to create a folder, script, material or text file."));
    }

    // Создание ассетов — ПКМ по пустому месту (не по тайлу: у тайлов своё меню).
    // Отступы темы для меню: всплывающее окно наследует стиль, действующий в
    // момент открытия (см. Sage::UI::MenuScope). Время жизни MenuScope
    // держится в границах if — иначе деструктор снялся бы после EndChild()/
    // End() этого окна, и ImGui решил бы, что стиль не сняли вовсе.
    if (Sage::UI::MenuScope createCtxMenu; ImGui::BeginPopupContextWindow("##assets_create_ctx",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        // ОДИН ПУНКТ НА ВИД, настройки — в окне (CreateAssetDialog.h): язык
        // скрипта, цвет папки. Пункта «скрипт» было два («.lua» и «.slua»), и
        // язык выбирали по расширению в меню, не видя, чем они различаются.
        // Создаётся в открытой папке; сцена — всегда в assets/Scenes.
        namespace ca = sage::editor::createasset;
        auto startCreate = [this, &host](ca::What kind, const char* defaultName) {
            ca::Request req;
            req.Kind = kind;
            req.Name = defaultName;
            req.Dir = kind == ca::What::Scene && host.CurrentProject().Loaded() ? host.CurrentProject().ScenesDir()
                                                                                : host.AssetsCwd();
            req.Done = [this, &host, kind](const fs::path& made) {
                if (kind == ca::What::Scene) host.AssetsCwd() = made.parent_path();
                m_selected = made;
                m_multi = {made};
            };
            ca::Open(req);
        };
        if (EditorIcons::MenuItem("folder-plus", T("Folder..."))) startCreate(ca::What::Folder, "NewFolder");
        if (EditorIcons::MenuItem("scene", T("Scene..."))) startCreate(ca::What::Scene, "NewScene");
        if (EditorIcons::MenuItem("script", T("Script..."))) startCreate(ca::What::Script, "NewScript");
        if (EditorIcons::MenuItem("material", T("Material..."))) startCreate(ca::What::Material, "NewMaterial");
        if (EditorIcons::MenuItem("file", T("Text file..."))) startCreate(ca::What::TextFile, "notes");
        ImGui::Separator();
        if (EditorIcons::MenuItem("refresh", T("Convert the whole folder to engine formats"))) ConvertFolderHere(host);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Models and images in this folder (including nested) -> .sagemesh/.sagetex.\n"
              "Already converted files are skipped, sources stay in place."));
        }
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
        m_deleteTargets = m_multi;   // дальше — обычная модалка подтверждения
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

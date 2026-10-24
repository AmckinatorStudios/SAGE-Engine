// ---------------------------------------------------------------------------
// EditorLayer — проект, сцена и сборка игры.
//
// Здесь всё, что отвечает на вопрос «с чем мы работаем»: создать сцену,
// открыть и сохранить её, завести или открыть проект, собрать из него игру,
// показать спасённое после падения. Одна связная область: у неё общий предмет
// (файлы проекта на диске) и общий способ ошибаться — путь, которого нет, или
// файл, который не читается.
//
// Часть класса EditorLayer: объявления методов остались в EditorLayer.h, здесь
// только тела. Разбит он ровно потому, что дорос до двух с половиной тысяч
// строк, в которых рядом лежали сборка игры, отмена правки и раскладка окон —
// три области, у которых нет ничего общего, кроме имени класса.
// ---------------------------------------------------------------------------
#include "EditorLayer.h"
#include "CodeEditorApp.h"
#include "ProjectLauncher/ProjectDatabase.h"
#include "SceneCover.h"
#include "Thumbnails.h"
#include "sage/assets/Pack.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <cfloat>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API (создание раскладки по умолчанию)
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"

#include "EditorTheme.h"
#include "EditorIcons.h"
#include "ModelMaterialImport.h"
#include "sage/render/DebugView.h"
#include "sage/app/Application.h"
#include "sage/core/Paths.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Systems.h"
#include "sage/core/Version.h"
#include "sage/core/CrashHandler.h"
#include "sage/render/MeshRaycast.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/render/LightingUpload.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/gi/GI.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/SceneSerializer.h"
#include "Localization.h"

namespace fs = std::filesystem;

// Вопросы человеку при запуске и выходе: отчёт о падении, восстановление, несохранённое.
// Предложение восстановить сцену после падения или из автосохранения.
//
// Модалка, а не тихая загрузка: восстановленная сцена может быть НЕ той, над
// которой человек работал последней (например, он с тех пор открыл другой
// проект), и решать это должен он, а не редактор.
// ============================================================================
//  ОТЧЁТ О ПРОШЛОМ ПАДЕНИИ
//
//  Обработчик падений пишет подробный отчёт (сигнал, стек, версия, открытый
//  проект, последние строки лога) — и до сих пор об этом знал только тот, кто
//  догадался поискать рядом с редактором файл sage-crash-*.txt. То есть отчёт
//  был, а человек, ради которого он писался, его не видел: он видел, что
//  «редактор закрылся сам».
//
//  Показать отчёт В МОМЕНТ падения нельзя: процесс уже разрушен, и рисовать
//  ImGui в нём — верный способ упасть второй раз, потеряв и отчёт. Поэтому в
//  момент падения — короткое системное окно (см. CrashHandler.cpp), а весь
//  текст — здесь, при следующем запуске, когда его есть чем показать.
// ============================================================================
void EditorLayer::DrawCrashReport() {
    if (!m_recovery.HasCrashReport()) return;
    // Одно имя на оба вызова и просьба ОДИН раз — см. DrawRecoveryPrompt.
    const char* const kId = T("Previous session crashed" "###crash-report");
    if (!ImGui::IsPopupOpen(kId)) ImGui::OpenPopup(kId);
    const ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::min(900.0f, vp.x * 0.8f), std::min(620.0f, vp.y * 0.8f)),
                             ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    ImGui::TextWrapped("%s", T("The editor did not close normally last time. Below is the full "
                               "report written at that moment — it is what a developer needs to "
                               "find the cause."));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", m_recovery.CrashReportPath().c_str());
    if (m_recovery.CrashReportCount() > 1)
        ImGui::TextDisabled(T("Reports next to the editor: %d"), (int)m_recovery.CrashReportCount());
    ImGui::Spacing();

    // Текст — в поле ввода только для чтения, а не Text(): так его можно
    // выделить мышью, прокрутить и скопировать кусок, а не только целиком.
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f;
    ImGui::InputTextMultiline("##crashtext", m_recovery.CrashReportText().data(),
                              m_recovery.CrashReportText().size() + 1,
                              ImVec2(-FLT_MIN, -footer), ImGuiInputTextFlags_ReadOnly);

    if (ImGui::Button(T("Copy report"), ImVec2(160, 0))) {
        ImGui::SetClipboardText(m_recovery.CrashReportText().c_str());
        SetStatusMessage(T("Crash report copied to the clipboard"));
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Copy path"), ImVec2(140, 0))) {
        ImGui::SetClipboardText(m_recovery.CrashReportPath().c_str());
        SetStatusMessage(T("Path copied to the clipboard"));
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Close"), ImVec2(120, 0))) {
        // Файл ОСТАЁТСЯ. Отчёт — единственный след падения, и стирать его
        // кнопкой «закрыть» значило бы отнимать у человека возможность его
        // прислать, когда он до этого дойдёт.
        m_recovery.DismissCrashReport();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (EditorTheme::ColoredButton(T("Delete report"), EditorTheme::Role::Danger, ImVec2(150, 0))) {
        std::error_code ec;
        m_recovery.DeleteCrashReport();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}


void EditorLayer::DrawRecoveryPrompt() {
    if (!m_recovery.HasRecoveryFile()) return;
    // ИМЯ ОКНА — ЦЕЛИКОМ И ОДНО И ТО ЖЕ В ОБОИХ ВЫЗОВАХ, вместе с «###».
    //
    // Здесь стоял OpenPopup(T("Restore scene?")) — БЕЗ «###». По-английски это
    // работало случайно: ImGui считает имя окна хэшем, сбрасывая его на «###»,
    // и у строки без «###» хэш совпадал с хвостом «###Restore scene?». А в
    // переводе OpenPopup получал «Восстановить сцену?» — другой хэш, то есть
    // ОТКРЫВАЛОСЬ ОДНО ОКНО, А РИСОВАЛОСЬ ДРУГОЕ.
    //
    // И это не «диалог не показался». Открытое окно, которое никто не рисует,
    // навсегда остаётся в стеке всплывающих окон ImGui, а раз модалка не
    // нарисована, предложение не сбросить — значит OpenPopup зовётся
    // КАЖДЫЙ КАДР. Каждый такой вызов закрывает всё, что человек открыл выше
    // (ImGui::OpenPopupEx -> ClosePopupToLevel), и меню «Файл» захлопывается в
    // том же кадре, в котором открылось. Снаружи это выглядит как «редактор
    // завис, кнопки не нажимаются»: картинка живая, вьюпорт крутится, а ни
    // одно меню и ни один диалог не открываются — и ни строчки в логе.
    //
    // Ловится это ДВУМЯ сторожами, потому что случай возвращается: одно и то же
    // имя в обоих вызовах проверяет scripts/check_popup_ids.py, а «открыто
    // окно, которое никто не рисует» — проверка кадра (CheckGhostPopups).
    const char* const kId = T("Restore scene?" "###Restore scene?");
    // Просим ОДИН раз, а не каждый кадр: повторный OpenPopup — это удар по
    // чужим открытым окнам, и платить им за то, что наше уже открыто, не за что.
    if (!ImGui::IsPopupOpen(kId)) ImGui::OpenPopup(kId);
    if (ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T("The previous session seems to have crashed"));
        ImGui::Spacing();
        ImGui::Text(T("Found file: %s"), m_recovery.RecoveryFile().c_str());
        ImGui::TextDisabled("%s", T("It did not overwrite your scene — this is a separate copy."));
        ImGui::Spacing();
        if (ImGui::Button(T("Open the copy"), ImVec2(150, 0))) {
            if (LoadSceneFromFile(m_recovery.RecoveryFile())) {
                // Путь сцены НЕ ставим: иначе первое же Ctrl+S записало бы
                // восстановленное поверх файла восстановления, а не сцены.
                m_scenePath.clear();
                m_sceneDirty = true;
                UpdateWindowTitle();
            }
            m_recovery.DismissRecovery();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Keep the file"), ImVec2(150, 0))) {
            m_recovery.DismissRecovery();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Delete"), ImVec2(110, 0))) {
            m_recovery.DeleteRecoveryFile();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}


// Окно попросили закрыть. Пока сцена не сохранена — не закрываемся, а
// спрашиваем; закроет приложение сам ответ (см. DrawUnsavedPrompt).
//
// Ровно ЗДЕСЬ, а не в кадре: флаг закрытия GLFW ставит в конце кадра, и
// перехват внутри отрисовки не получал управления вовсе — редактор закрывался
// молча вместе с несохранённой сценой (см. Layer::OnCloseRequest).
bool EditorLayer::OnCloseRequest() {
    if (!m_sceneDirty) return true;
    if (!m_unsavedPrompt) {
        m_closeAfterPrompt = true;
        AskUnsaved(nullptr);   // «продолжить» здесь и значит «закрыться»
    }
    return false;
}


// Спросить про несохранённую сцену и, если ответят, сделать action.
//
// Сохранено — спрашивать не о чем, действие идёт сразу: вопрос, который задают
// всегда, перестают читать.
void EditorLayer::AskUnsaved(std::function<void()> action) {
    if (!m_sceneDirty) {
        if (action) action();
        return;
    }
    m_afterUnsaved = std::move(action);
    m_unsavedPrompt = true;
}


// ТРИ ОТВЕТА, А НЕ ДВА. «Сохранить» и «Отмена» — это ловушка для того, кто
// действительно хочет выбросить черновик: он вынужден сохранять мусор поверх
// хорошего файла. Поэтому «Сохранить», «Не сохранять» и «Отмена» — ровно тот
// набор, который человек видел во всех программах, где что-то редактируют.
void EditorLayer::DrawUnsavedPrompt() {
    if (!m_unsavedPrompt) return;
    // Одно имя на оба вызова и просьба ОДИН раз — см. DrawRecoveryPrompt.
    const char* const kId = T("Scene not saved" "###Unsaved");
    if (!ImGui::IsPopupOpen(kId)) ImGui::OpenPopup(kId);
    if (ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string name = m_scenePath.empty() ? CurrentSceneName()
                                                     : m_scenePath.filename().string();
        ImGui::Text(T("Scene \"%s\" has unsaved changes."), name.c_str());
        ImGui::TextDisabled("%s", T("They will be lost if you continue."));
        ImGui::Spacing();

        auto finish = [&](bool proceed) {
            m_unsavedPrompt = false;
            const bool closing = m_closeAfterPrompt;
            m_closeAfterPrompt = false;
            auto action = std::move(m_afterUnsaved);
            m_afterUnsaved = nullptr;
            ImGui::CloseCurrentPopup();
            if (!proceed) return;
            if (action) action();
            if (closing) sage::Application::Get().Close();
        };

        if (ImGui::Button(T("Save"), ImVec2(150, 0))) {
            if (m_scenePath.empty()) {
                // Имени у сцены ещё нет — спрашиваем его, а отложенное действие
                // отменяем: продолжать, не дождавшись записи, значит потерять
                // ровно то, что человек попросил сохранить.
                m_unsavedPrompt = false;
                m_closeAfterPrompt = false;
                m_afterUnsaved = nullptr;
                RequestDialog("Save Scene As");
                ImGui::CloseCurrentPopup();
            } else if (SaveSceneToFile(m_scenePath)) {
                finish(true);
            } else {
                // Запись не удалась — причина в консоли, а выход отменяем:
                // закрыться после неудачной записи значит потерять сцену.
                finish(false);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Don't save"), ImVec2(150, 0))) finish(true);
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(110, 0))) finish(false);
        ImGui::EndPopup();
    }
}

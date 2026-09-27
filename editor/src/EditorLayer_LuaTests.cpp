// ---------------------------------------------------------------------------
// EditorLayer — тесты на Lua в настоящем Play-режиме редактора.
//
// Модульные тесты движка проверяют функции по одной, в пустой сцене и без
// кадра. Скрипт игры так не живёт: он работает в Play, рядом с физикой,
// интерфейсом, вводом и чужими скриптами, и ломается как раз на их стыке —
// «сигнал пришёл на кадр позже», «кнопка не нажимается, пока панель скрыта»,
// «время стоит в паузе». Поэтому тесты на Lua идут ЗДЕСЬ: тем же Play, тем же
// кадром, той же системой скриптинга, что и игра, которую человек запускает
// кнопкой.
//
// Тест — обычный скрипт объекта в файле *.test.lua с методами test_* (см.
// sage/scripting/ScriptTests.h). Раннер ставит по объекту на файл, прогоняет
// тесты корутинами по кадрам и пишет итог в консоль. Play, запущенный ради
// тестов, останавливается сам — сцена возвращается ровно такой, какой была.
// ---------------------------------------------------------------------------
#include "EditorLayer.h"

#include <algorithm>
#include <filesystem>

#include "Localization.h"
#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scripting/ScriptComponent.h"
#include "sage/scripting/ScriptingSystem.h"

namespace fs = std::filesystem;

std::vector<std::string> EditorLayer::FindLuaTestFiles() const {
    std::vector<std::string> out;
    if (!m_project.Loaded()) return out;
    std::error_code ec;
    const fs::path root = m_project.Dir();
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        const std::string name = p.filename().string();
        // Служебные папки (".sage" с обложками и кэшем) и собранные игры —
        // не тесты: в сборке лежит копия проекта, и её тесты шли бы дважды.
        if (it->is_directory(ec) && (name.empty() || name[0] == '.' || name == "Builds")) {
            it.disable_recursion_pending();
            continue;
        }
        const std::string suffix = ".test.lua";
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(m_project.AssetRef(p));
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool EditorLayer::RunLuaTests(const std::vector<std::string>& requested) {
    if (m_luaTests.Running) return false;
    const std::vector<std::string> files = requested.empty() ? FindLuaTestFiles() : requested;
    m_luaTests = LuaTestRun{};
    if (files.empty()) {
        SetStatusMessage(T("No Lua tests: add files named *.test.lua to the project"));
        LOG_WARN("Test") << "Тестов на Lua нет: в проекте нет файлов *.test.lua";
        return false;
    }

    // Play — тот же, что по кнопке. Уже идёт — тесты встают в него: иногда
    // проверить надо именно то состояние, до которого человек доиграл.
    if (!m_play.Active()) {
        StartPlay();
        m_luaTests.StartedPlay = m_play.Active();
    }
    sage::scripting::ScriptingSystem* scripting = m_play.Scripting();
    if (!m_play.Active() || !scripting || !m_scene) {
        LOG_ERROR("Test") << "Тесты на Lua не запущены: Play не стартовал";
        return false;
    }
    scripting->ClearTestResults();

    LOG_INFO("Test") << "Тесты на Lua: файлов " << files.size();
    for (const std::string& file : files) {
        // Объект на файл: тест — обычный скрипт объекта, и у него есть всё, что
        // есть у скрипта в игре (self, публичные поля, сигналы своего объекта).
        GameObject runner = m_scene->CreateObject("LuaTest · " + fs::path(file).filename().string());
        m_scene->Registry().emplace_or_replace<ScriptComponent>(runner.Entity()).Path = file;
        if (!scripting->Attach(runner)) {
            // Файл не собрался — это провал теста, а не «тестов нет».
            ++m_luaTests.Failed;
            m_luaTests.Failures.push_back(file + ": скрипт не загрузился (причина — выше в консоли)");
            continue;
        }
        const int n = scripting->RunTests(runner);
        if (n == 0) LOG_WARN("Test") << file << ": нет методов test_*";
        m_luaTests.Queued += n;
    }
    m_luaTests.Running = true;
    SetStatusMessage(T("Running Lua tests…"));
    return true;
}

void EditorLayer::TickLuaTests() {
    if (!m_luaTests.Running) return;
    sage::scripting::ScriptingSystem* scripting = m_play.Scripting();
    // Play остановили посреди прогона (кнопкой или скриптом) — прогон кончился.
    const bool aborted = !m_play.Active() || !scripting;
    if (!aborted && scripting->PendingTests() > 0) return;

    if (scripting) {
        for (const sage::scripting::TestResult& r : scripting->TestResults()) {
            if (r.Passed) {
                ++m_luaTests.Passed;
            } else {
                ++m_luaTests.Failed;
                m_luaTests.Failures.push_back((r.File.empty() ? r.Script : r.File) +
                                              (r.Line > 0 ? ":" + std::to_string(r.Line) : std::string()) +
                                              ": " + r.Name + " — " + r.Message);
            }
        }
    }
    if (aborted) {
        const int lost = m_luaTests.Queued - m_luaTests.Passed - (int)m_luaTests.Failures.size();
        if (lost > 0) {
            m_luaTests.Failed += lost;
            m_luaTests.Failures.push_back("Play остановлен посреди тестов — не доиграно: " + std::to_string(lost));
        }
    }
    m_luaTests.Running = false;
    m_luaTests.Finished = true;

    const int total = m_luaTests.Passed + m_luaTests.Failed;
    if (m_luaTests.Failed == 0) {
        LOG_INFO("Test") << "Тесты на Lua: все прошли (" << m_luaTests.Passed << ")";
        SetStatusMessage(std::string(T("Lua tests passed: ")) + std::to_string(m_luaTests.Passed));
    } else {
        LOG_ERROR("Test") << "Тесты на Lua: провалено " << m_luaTests.Failed << " из " << total;
        SetStatusMessage(std::string(T("Lua tests failed: ")) + std::to_string(m_luaTests.Failed) + " / " +
                         std::to_string(total));
    }
    m_panels[EditorPanel::Console] = true;
    if (m_luaTests.StartedPlay && m_play.Active()) StopPlay();
}

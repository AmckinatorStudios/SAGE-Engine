#include "LogicKinds.h"

#include <fstream>

#include "EditorHost.h"
#include "panels/AssetsPanel.h"
#include "sage/ai/Graph.h"
#include "sage/core/Paths.h"

// Встроенные виды логики редактора (см. LogicKinds.h): SAGE Lua, Script Graph,
// AI Graph. Отдельно от самого списка: список нужен и тестам плагинов, а
// создание файлов тянет панель Assets и редакторы графов.
namespace fs = std::filesystem;

namespace sage::editor::logic {

namespace {

// Новый граф — сразу с точкой начала своего вида: Script Graph — с событиями
// объекта On Start и On Update, AI Graph — с Task Start, откуда поведение
// начинается и чьей веткой кончается. Пустой лист не подсказывает, откуда
// тянуть первый провод, а без начала граф и не запустится.
bool WriteGraph(sage::ai::GraphKind kind, const std::string& name, const fs::path& dir, fs::path& created,
                std::string& err) {
    if (name.empty()) {
        err = "Name must not be empty";
        return false;
    }
    const char* ext = sage::ai::GraphFileExt(kind);
    fs::path target = dir / sage::PathFromUtf8(name);
    if (target.extension() != ext) target += ext;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        err = "Already exists: " + sage::PathToUtf8(target.filename());
        return false;
    }
    fs::create_directories(dir, ec);
    sage::ai::GraphDoc d;
    d.Kind = kind;
    d.Name = sage::PathToUtf8(target.stem());
    if (kind == sage::ai::GraphKind::Behavior) {
        d.AddNode("ai.task_start", 0, 0);
    } else {
        d.AddNode("script.on_start", 0, 0);
        d.AddNode("script.on_update", 0, 200);
    }
    std::ofstream f(target, std::ios::binary);
    f << sage::ai::SaveGraph(d);
    if (!f) {
        err = "Could not write " + sage::PathToUtf8(target);
        return false;
    }
    created = target;
    return true;
}

std::vector<Kind> Builtins() {
    std::vector<Kind> v;
    {
        Kind k;
        k.Id = "slua";
        k.Title = "SAGE Lua";
        k.Note = "Text script for game logic";
        k.Ext = ".slua";
        k.Folder = "scripts";
        k.Slot = "script";
        k.Group = "scripts";
        k.DefaultName = "NewScript";
        k.Create = [](const std::string& name, const fs::path& dir, fs::path& created, std::string& err) {
            std::error_code ec;
            fs::create_directories(dir, ec);   // своя папка вида может ещё не существовать
            return AssetsPanel::CreateAsset(AssetsPanel::CreateKind::SageScript, name, dir, created, err);
        };
        v.push_back(std::move(k));
    }
    {
        Kind k;
        k.Id = "sgraph";
        k.Title = "Script Graph";
        k.Note = "Visual game logic: events, conditions, actions";
        k.Ext = ".sgraph";
        k.Folder = "visual";
        k.Icon = "wire";
        k.Slot = "visual";
        k.Group = "scripts";
        k.DefaultName = "NewScriptGraph";
        k.Create = [](const std::string& name, const fs::path& dir, fs::path& created, std::string& err) {
            return WriteGraph(sage::ai::GraphKind::Script, name, dir, created, err);
        };
        k.Open = [](EditorHost& h, const fs::path& p) { h.OpenVisualScript(sage::PathToUtf8(p)); };
        v.push_back(std::move(k));
    }
    {
        Kind k;
        k.Id = "aigraph";
        k.Title = "AI Graph";
        k.Note = "AI behavior: tasks, conditions, priorities";
        k.Ext = ".aigraph";
        k.Folder = "ai";
        // Поведение зовут по имени (Run Task) — библиотека ИИ ищет его в assets/ai.
        k.OwnFolderOnly = true;
        k.Icon = "wire";
        k.Slot = "ai";
        k.Group = "ai";
        k.DefaultName = "NewAIGraph";
        k.Create = [](const std::string& name, const fs::path& dir, fs::path& created, std::string& err) {
            return WriteGraph(sage::ai::GraphKind::Behavior, name, dir, created, err);
        };
        k.Open = [](EditorHost& h, const fs::path& p) { h.OpenAiGraph(sage::PathToUtf8(p)); };
        v.push_back(std::move(k));
    }
    return v;
}

} // namespace

void RegisterBuiltins() {
    for (Kind& k : Builtins()) Register(std::move(k));
}

} // namespace sage::editor::logic

#include "LogicKinds.h"

#include <fstream>

#include "EditorHost.h"
#include "panels/AssetsPanel.h"
#include "sage/ai/Graph.h"
#include "sage/core/Paths.h"

// Встроенные виды логики редактора (см. LogicKinds.h). Отдельно от самого
// списка: список нужен и тестам плагинов, а создание файлов тянет панель
// Assets и редакторы графов.
namespace fs = std::filesystem;

namespace sage::editor::logic {

namespace {

// Граф на диск: пустой лист с входом, с которого граф начинается, — иначе
// открытый граф молчит, и непонятно, откуда тянуть первый провод.
bool WriteGraph(sage::ai::GraphKind kind, const std::string& name, const fs::path& dir, const char* ext,
                fs::path& created, std::string& err) {
    if (name.empty()) {
        err = "Name must not be empty";
        return false;
    }
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
    if (kind == sage::ai::GraphKind::Script) {
        d.AddNode("script.on_start", 0, 0);
        d.AddNode("script.on_update", 0, 200);
    } else {
        d.AddNode("flow.start", 0, 0);
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
    auto script = [](AssetsPanel::CreateKind ck) {
        return [ck](const std::string& name, const fs::path& dir, fs::path& created, std::string& err) {
            std::error_code ec;
            fs::create_directories(dir, ec);   // своя папка вида может ещё не существовать
            return AssetsPanel::CreateAsset(ck, name, dir, created, err);
        };
    };
    {
        Kind k;
        k.Id = "lua";
        k.Title = "Lua (.lua)";
        k.Note = "Plain Lua: quick to write, errors show up while the game runs";
        k.Ext = ".lua";
        k.Folder = "scripts";
        k.Slot = "script";
        k.Create = script(AssetsPanel::CreateKind::Script);
        v.push_back(std::move(k));
    }
    {
        Kind k;
        k.Id = "slua";
        k.Title = "SAGE Lua (.slua)";
        k.Note = "Lua with types and classes: mistakes are caught before the script runs";
        k.Ext = ".slua";
        k.Folder = "scripts";
        k.Slot = "script";
        k.Create = script(AssetsPanel::CreateKind::SageScript);
        v.push_back(std::move(k));
    }
    auto graph = [](sage::ai::GraphKind gk, const char* ext) {
        return [gk, ext](const std::string& name, const fs::path& dir, fs::path& created, std::string& err) {
            return WriteGraph(gk, name, dir, ext, created, err);
        };
    };
    {
        Kind k;
        k.Id = "visual";
        k.Title = "Visual Script (.vgraph)";
        k.Note = "Object logic made of nodes: On Start, On Update, events";
        k.Ext = ".vgraph";
        k.Folder = "visual";
        k.Icon = "wire";
        k.Slot = "visual";
        k.Create = graph(sage::ai::GraphKind::Script, ".vgraph");
        k.Open = [](EditorHost& h, const fs::path& p) { h.OpenVisualScript(sage::PathToUtf8(p)); };
        v.push_back(std::move(k));
    }
    {
        Kind k;
        k.Id = "task";
        k.Title = "Task for AI (.vgraph)";   // переводится при показе
        k.Note = "How an AI task is done, in nodes: AI Graph runs it by name";
        k.Ext = ".vgraph";
        k.Folder = "visual";
        k.OwnFolderOnly = true;
        k.Icon = "wire";
        k.Slot = "task";
        k.Create = graph(sage::ai::GraphKind::Task, ".vgraph");
        k.Open = [](EditorHost& h, const fs::path& p) { h.OpenVisualScript(sage::PathToUtf8(p)); };
        v.push_back(std::move(k));
    }
    {
        Kind k;
        k.Id = "ai";
        k.Title = "AI Graph (.aigraph)";
        k.Note = "Decisions of a character: which task to run and when to switch";
        k.Ext = ".aigraph";
        k.Folder = "ai";
        k.OwnFolderOnly = true;
        k.Icon = "wire";
        k.Slot = "ai";
        k.Create = graph(sage::ai::GraphKind::Behavior, ".aigraph");
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

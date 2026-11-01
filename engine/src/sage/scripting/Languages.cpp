#include "sage/scripting/Languages.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <mutex>

#include "sage/core/Log.h"
#include "sage/lang/SageCompiler.h"
#include "sage/scripting/ScriptFields.h"
#include "sage/scripting/ScriptingSystem.h"
#include "sage/scripting/lua/LuaBackend.h"
#include "sage/scripting/lua/LuaInternal.h"

namespace sage::scripting {

namespace {

std::mutex& Mutex() {
    static std::mutex m;
    return m;
}
std::vector<Language>& List() {
    static std::vector<Language> list;
    return list;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string ExtensionOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return Lower(path.substr(dot));
}

// Регистрация под замком; проверка занятых имён и расширений.
bool AddLocked(Language language, std::string* err) {
    for (std::string& e : language.Extensions) e = Lower(e);
    std::vector<Language>& list = List();
    for (Language& l : list) {
        if (l.Id == language.Id) {
            l = std::move(language);   // повтор — замена (перечитали файл проекта)
            return true;
        }
    }
    for (const Language& l : list) {
        for (const std::string& e : language.Extensions) {
            if (std::find(l.Extensions.begin(), l.Extensions.end(), e) != l.Extensions.end()) {
                if (err) *err = "расширение " + e + " уже за языком «" + l.Name + "»";
                return false;
            }
        }
    }
    list.push_back(std::move(language));
    return true;
}

// Встроенные языки — обычные записи. Регистрируются при первом обращении к
// реестру: статическая инициализация в другом порядке оставила бы реестр
// пустым ровно тогда, когда его спросили.
void EnsureBuiltins() {
    static std::once_flag once;
    std::call_once(once, [] {
        Language luaLang;
        luaLang.Id = "lua";
        luaLang.Name = "Lua";
        luaLang.Extensions = {".lua"};
        luaLang.CreateBackend = [](const LanguageHost& host) { return MakeLuaBackend({host.Engine}); };
        luaLang.Fields = [](const std::string&, const std::string& src) { return lua::ParsePublicFields(src); };
        luaLang.Methods = [](const std::string&, const std::string& src) { return ParseLuaMethods(src); };
        luaLang.Check = [](const std::string& path, const std::string& src) { return LintScriptSource(path, src); };

        // SAGE Lua — компилятор в Lua: исполняет Lua-бэкенд, поля и методы —
        // из объявления класса, которое уже проверил компилятор (а не по
        // шаблону текста). Файл с ошибкой полей не даёт: инспектор не
        // показывает поля, которых, возможно, нет.
        Language slua;
        slua.Id = "sage-lua";
        slua.Name = "SAGE Lua";
        slua.Extensions = {".slua"};
        slua.RunsOn = "lua";
        slua.Fields = [](const std::string& path, const std::string& src) {
            auto r = sage::lang::CompileForEngine(path, src);
            return r->Ok ? sage::lang::FieldsOf(*r) : sage::vars::Table{};
        };
        slua.Methods = [](const std::string& path, const std::string& src) {
            auto r = sage::lang::CompileForEngine(path, src);
            return r->Ok ? sage::lang::MethodsOf(*r) : std::vector<std::string>{};
        };
        slua.Check = [](const std::string& path, const std::string& src) { return LintScriptSource(path, src); };

        std::lock_guard<std::mutex> lock(Mutex());
        AddLocked(std::move(luaLang), nullptr);
        AddLocked(std::move(slua), nullptr);
    });
}

} // namespace

bool RegisterLanguage(Language language, std::string* err) {
    EnsureBuiltins();
    if (language.Id.empty() || language.Extensions.empty()) {
        if (err) *err = "у языка нет имени или расширений";
        return false;
    }
    std::lock_guard<std::mutex> lock(Mutex());
    return AddLocked(std::move(language), err);
}

bool UnregisterLanguage(const std::string& id) {
    EnsureBuiltins();
    std::lock_guard<std::mutex> lock(Mutex());
    std::vector<Language>& list = List();
    const size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Language& l) { return l.Id == id; }), list.end());
    return list.size() != before;
}

int UnregisterLanguages(const std::string& origin) {
    EnsureBuiltins();
    std::lock_guard<std::mutex> lock(Mutex());
    std::vector<Language>& list = List();
    const size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Language& l) { return l.Origin == origin; }),
               list.end());
    return (int)(before - list.size());
}

std::vector<Language> Languages() {
    EnsureBuiltins();
    std::lock_guard<std::mutex> lock(Mutex());
    return List();
}

bool FindLanguage(const std::string& id, Language& out) {
    EnsureBuiltins();
    std::lock_guard<std::mutex> lock(Mutex());
    for (const Language& l : List()) {
        if (l.Id == id) {
            out = l;
            return true;
        }
    }
    return false;
}

bool LanguageForPath(const std::string& path, Language& out) {
    EnsureBuiltins();
    const std::string ext = ExtensionOf(path);
    if (ext.empty()) return false;
    std::lock_guard<std::mutex> lock(Mutex());
    for (const Language& l : List()) {
        if (std::find(l.Extensions.begin(), l.Extensions.end(), ext) != l.Extensions.end()) {
            out = l;
            return true;
        }
    }
    return false;
}

bool IsScriptPath(const std::string& path) {
    Language l;
    return LanguageForPath(path, l);
}

std::vector<std::string> ScriptExtensions() {
    std::vector<std::string> out;
    for (const Language& l : Languages()) out.insert(out.end(), l.Extensions.begin(), l.Extensions.end());
    return out;
}

int AttachLanguages(ScriptingSystem& system, const LanguageHost& host) {
    int made = 0;
    for (const Language& l : Languages()) {
        if (!l.CreateBackend) continue;
        try {
            if (std::unique_ptr<LanguageBackend> backend = l.CreateBackend(host)) {
                system.AddBackend(std::move(backend));
                ++made;
            }
        } catch (const std::exception& e) {
            // Сбой одного языка (нет интерпретатора, битое описание) не
            // оставляет игру без остальных.
            LOG_ERROR("Scripting") << "язык «" << l.Name << "» не подключён: " << e.what();
        }
    }
    return made;
}

} // namespace sage::scripting
